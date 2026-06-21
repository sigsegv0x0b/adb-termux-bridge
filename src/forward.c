#include "forward.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define MAX_FORWARDS 32
#define RELAY_BUF 65536

struct tcpforward {
    int      id;
    char     unix_path[256];
    char     host[64];
    int      port;
    int      secure;
    int      listen_fd;
    pthread_t thread;
    volatile int running;
    int      active_connections;
    struct tcpforward *next;
};

static pthread_mutex_t g_forward_mutex = PTHREAD_MUTEX_INITIALIZER;
static tcpforward_t *g_forwards = NULL;
static int g_next_id = 1;

typedef struct {
    int fd_a;
    int fd_b;
    tcpforward_t *fw;
} relay_job_t;

static int connect_unix(const char *path) {
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void *relay_thread(void *arg) {
    relay_job_t *job = (relay_job_t *)arg;
    int fd_a = job->fd_a;
    int fd_b = job->fd_b;

    char buf[RELAY_BUF];

    while (1) {
        struct pollfd fds[2] = {
            { .fd = fd_a, .events = POLLIN },
            { .fd = fd_b, .events = POLLIN }
        };

        int ret = poll(fds, 2, 500);
        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }

        if (fds[0].revents & (POLLIN | POLLHUP)) {
            ssize_t n = read(fd_a, buf, sizeof(buf));
            if (n > 0) {
                ssize_t written = 0;
                while (written < n) {
                    ssize_t w = write(fd_b, buf + written, n - written);
                    if (w <= 0) goto cleanup;
                    written += w;
                }
            } else {
                break;
            }
        }

        if (fds[1].revents & (POLLIN | POLLHUP)) {
            ssize_t n = read(fd_b, buf, sizeof(buf));
            if (n > 0) {
                ssize_t written = 0;
                while (written < n) {
                    ssize_t w = write(fd_a, buf + written, n - written);
                    if (w <= 0) goto cleanup;
                    written += w;
                }
            } else {
                break;
            }
        }

        if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) break;
        if (fds[1].revents & (POLLERR | POLLHUP | POLLNVAL)) break;
    }

cleanup:
    close(fd_a);
    close(fd_b);

    pthread_mutex_lock(&g_forward_mutex);
    if (job->fw->active_connections > 0)
        job->fw->active_connections--;
    pthread_mutex_unlock(&g_forward_mutex);

    free(job);
    return NULL;
}

static void *accept_thread(void *arg) {
    tcpforward_t *fw = (tcpforward_t *)arg;

    while (fw->running) {
        struct pollfd pfd = { .fd = fw->listen_fd, .events = POLLIN };
        int ret = poll(&pfd, 1, 500);
        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (ret == 0) continue;

        int client_fd = accept4(fw->listen_fd, NULL, NULL, SOCK_CLOEXEC);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            if (!fw->running) break;
            continue;
        }

        int unix_fd = connect_unix(fw->unix_path);
        if (unix_fd < 0) {
            close(client_fd);
            usleep(100000);
            continue;
        }

        relay_job_t *job = malloc(sizeof(relay_job_t));
        if (!job) {
            close(client_fd);
            close(unix_fd);
            continue;
        }
        job->fd_a = client_fd;
        job->fd_b = unix_fd;
        job->fw = fw;

        pthread_mutex_lock(&g_forward_mutex);
        fw->active_connections++;
        pthread_mutex_unlock(&g_forward_mutex);

        pthread_t tid;
        pthread_create(&tid, NULL, relay_thread, job);
        pthread_detach(tid);
    }

    if (fw->listen_fd >= 0) {
        close(fw->listen_fd);
        fw->listen_fd = -1;
    }
    return NULL;
}

int forward_add(const char *unix_path, const char *host, int port, int secure) {
    pthread_mutex_lock(&g_forward_mutex);

    int count = 0;
    for (tcpforward_t *f = g_forwards; f; f = f->next) {
        if (f->port == port && strcmp(f->host, host) == 0) {
            pthread_mutex_unlock(&g_forward_mutex);
            return -1;
        }
        if (strcmp(f->unix_path, unix_path) == 0 && f->port == port) {
            pthread_mutex_unlock(&g_forward_mutex);
            return -2;
        }
        count++;
    }
    if (count >= MAX_FORWARDS) {
        pthread_mutex_unlock(&g_forward_mutex);
        return -3;
    }

    int listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listen_fd < 0) {
        pthread_mutex_unlock(&g_forward_mutex);
        return -4;
    }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, host, &addr.sin_addr);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(listen_fd);
        pthread_mutex_unlock(&g_forward_mutex);
        return -5;
    }

    if (listen(listen_fd, 32) < 0) {
        close(listen_fd);
        pthread_mutex_unlock(&g_forward_mutex);
        return -6;
    }

    tcpforward_t *fw = calloc(1, sizeof(tcpforward_t));
    if (!fw) {
        close(listen_fd);
        pthread_mutex_unlock(&g_forward_mutex);
        return -7;
    }

    fw->id = g_next_id++;
    strncpy(fw->unix_path, unix_path, sizeof(fw->unix_path) - 1);
    strncpy(fw->host, host, sizeof(fw->host) - 1);
    fw->port = port;
    fw->secure = secure;
    fw->listen_fd = listen_fd;
    fw->running = 1;
    fw->active_connections = 0;
    fw->next = g_forwards;
    g_forwards = fw;

    pthread_create(&fw->thread, NULL, accept_thread, fw);
    pthread_detach(fw->thread);

    int id = fw->id;
    pthread_mutex_unlock(&g_forward_mutex);
    return id;
}

int forward_remove(int id) {
    pthread_mutex_lock(&g_forward_mutex);

    tcpforward_t *prev = NULL;
    for (tcpforward_t *f = g_forwards; f; f = f->next) {
        if (f->id == id) {
            f->running = 0;

            if (f->listen_fd >= 0) {
                close(f->listen_fd);
                f->listen_fd = -1;
            }

            if (prev) prev->next = f->next;
            else g_forwards = f->next;

            free(f);
            pthread_mutex_unlock(&g_forward_mutex);
            return 0;
        }
        prev = f;
    }

    pthread_mutex_unlock(&g_forward_mutex);
    return -1;
}

void forward_list_json(char *buf, size_t size) {
    pthread_mutex_lock(&g_forward_mutex);

    int pos = snprintf(buf, size, "{\"forwards\":[");
    int first = 1;

    for (tcpforward_t *f = g_forwards; f; f = f->next) {
        if (!first) pos += snprintf(buf + pos, size - pos, ",");
        first = 0;
        pos += snprintf(buf + pos, size - pos,
            "{\"id\":%d,\"unix_path\":\"%s\",\"host\":\"%s\",\"port\":%d,\"secure\":%s,\"connections\":%d}",
            f->id, f->unix_path, f->host, f->port,
            f->secure ? "true" : "false",
            f->active_connections);
    }

    snprintf(buf + pos, size - pos, "]}");
    pthread_mutex_unlock(&g_forward_mutex);
}

void forward_shutdown_all(void) {
    pthread_mutex_lock(&g_forward_mutex);

    for (tcpforward_t *f = g_forwards; f; f = f->next) {
        f->running = 0;
        if (f->listen_fd >= 0) {
            close(f->listen_fd);
            f->listen_fd = -1;
        }
    }

    pthread_mutex_unlock(&g_forward_mutex);
}
