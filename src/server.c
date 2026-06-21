#include "server.h"
#include "handler.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <openssl/err.h>

#define REQUEST_BUF 65536

typedef struct {
    SSL_CTX *ssl_ctx;
    int      fd;
} client_job_t;

static int read_until_crnlcrnl(SSL *ssl, char *buf, size_t cap, size_t *len) {
    *len = 0;
    while (*len < cap - 1) {
        int n = SSL_read(ssl, buf + *len, 1);
        if (n <= 0) return -1;
        (*len)++;
        buf[*len] = '\0';
        if (*len >= 4 && memcmp(buf + *len - 4, "\r\n\r\n", 4) == 0)
            return 0;
    }
    return -1;
}

static int parse_request_line(const char *line, char *method, size_t mcap,
                               char *path, size_t pcap, char *query, size_t qcap) {
    method[0] = path[0] = query[0] = '\0';
    const char *p = line;
    while (*p && *p != ' ' && p - line < (int)mcap - 1) {
        *method++ = *p++;
    }
    *method = '\0';
    if (*p != ' ') return -1;
    p++; // skip SP
    const char *q_start = NULL;
    while (*p && *p != ' ' && p - line < (int)(line + strlen(line) - line)) {
        if (*p == '?' && !q_start) {
            *path = '\0';
            path++;
            q_start = path;
            p++;
            continue;
        }
        if (q_start) {
            if (path - q_start < (int)qcap - 1) *path++ = *p;
        } else {
            if (path - line < (int)pcap - 1) *path++ = *p;
        }
        p++;
    }
    if (q_start) {
        *path = '\0';
        strncpy(query, q_start, qcap - 1);
        query[qcap - 1] = '\0';
    } else {
        *path = '\0';
    }
    return 0;
}

static long get_content_length(const char *headers) {
    const char *cl = strstr(headers, "Content-Length:");
    if (!cl) cl = strstr(headers, "content-length:");
    if (!cl) return -1;
    cl += 15;
    while (*cl == ' ') cl++;
    return atol(cl);
}

static int is_chunked(const char *headers) {
    const char *te = strstr(headers, "Transfer-Encoding:");
    if (!te) te = strstr(headers, "transfer-encoding:");
    if (!te) return 0;
    te += 18; // skip "Transfer-Encoding:" (18 chars including colon)
    while (*te == ' ') te++;
    return strncmp(te, "chunked", 7) == 0;
}

static void send_http_response(SSL *ssl, int status, const char *status_text,
                                const char *content_type, const char *body, size_t body_len) {
    char buf[4096];
    int n = snprintf(buf, sizeof(buf),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        status, status_text, content_type, body_len);
    SSL_write(ssl, buf, n);
    if (body_len > 0) SSL_write(ssl, body, body_len);
}

static void send_json_response(SSL *ssl, int status, const char *body) {
    send_http_response(ssl, status,
        status == 200 ? "OK" : status == 400 ? "Bad Request" : status == 404 ? "Not Found" : "Internal Server Error",
        "application/json", body, strlen(body));
}

static void send_error_json(SSL *ssl, int status, const char *msg) {
    char json[1024];
    snprintf(json, sizeof(json), "{\"error\":\"%s\"}", msg);
    send_json_response(ssl, status, json);
}

static void read_body(SSL *ssl, long content_length, char *buf, size_t cap, size_t *len) {
    *len = 0;
    if (content_length <= 0) return;
    if ((size_t)content_length >= cap) content_length = cap - 1;

    while (*len < (size_t)content_length) {
        int n = SSL_read(ssl, buf + *len, content_length - *len);
        if (n <= 0) break;
        *len += n;
    }
    buf[*len] = '\0';
}

static void *handle_client(void *arg) {
    client_job_t *job = (client_job_t *)arg;
    SSL *ssl = SSL_new(job->ssl_ctx);
    SSL_set_fd(ssl, job->fd);

    int ret = SSL_accept(ssl);
    if (ret != 1) {
        ERR_print_errors_fp(stderr);
        SSL_free(ssl);
        close(job->fd);
        free(job);
        return NULL;
    }

    if (SSL_get_verify_result(ssl) != X509_V_OK) {
        send_error_json(ssl, 403, "client certificate verification failed");
        SSL_free(ssl);
        close(job->fd);
        free(job);
        return NULL;
    }

    char buf[REQUEST_BUF];
    size_t header_len = 0;
    if (read_until_crnlcrnl(ssl, buf, sizeof(buf), &header_len) < 0) {
        SSL_free(ssl);
        close(job->fd);
        free(job);
        return NULL;
    }

    char method[16], path[1024], query[1024];
    if (parse_request_line(buf, method, sizeof(method), path, sizeof(path), query, sizeof(query)) < 0) {
        send_error_json(ssl, 400, "invalid request line");
        SSL_free(ssl);
        close(job->fd);
        free(job);
        return NULL;
    }

    long content_length = get_content_length(buf);

    // Handle Expect: 100-continue — send 100 Continue before any body-reading
    // endpoint so the client starts sending the request body
    if (strcmp(method, "POST") == 0 &&
        (strstr(buf, "Expect: 100-continue") || strstr(buf, "expect: 100-continue"))) {
        SSL_write(ssl, "HTTP/1.1 100 Continue\r\n\r\n", 25);
    }

    if (strcmp(path, "/api/upload") == 0 && strcmp(method, "POST") == 0) {
        handle_upload(ssl, query, content_length);
        SSL_shutdown(ssl);
        SSL_free(ssl);
        close(job->fd);
        free(job);
        return NULL;
    }

    int chunked = is_chunked(buf);
    if (strcmp(path, "/api/exec/pipe") == 0 && strcmp(method, "POST") == 0) {
        handle_exec_pipe(ssl, query, content_length, chunked);
        SSL_shutdown(ssl);
        SSL_free(ssl);
        close(job->fd);
        free(job);
        return NULL;
    }

    char body[REQUEST_BUF];
    size_t body_len = 0;
    read_body(ssl, content_length, body, sizeof(body), &body_len);

    int handled = handle_request(ssl, method, path, query, body, body_len);
    if (!handled) {
        send_error_json(ssl, 404, "not found");
    }

    SSL_shutdown(ssl);
    SSL_free(ssl);
    close(job->fd);
    free(job);
    return NULL;
}

static void kill_previous_instance(void) {
    DIR *proc = opendir("/proc");
    if (!proc) return;

    pid_t my_pid = getpid();
    struct dirent *entry;
    while ((entry = readdir(proc)) != NULL) {
        char *end;
        pid_t pid = strtol(entry->d_name, &end, 10);
        if (*end != '\0' || pid == my_pid || pid <= 0) continue;

        char cmdline_path[64];
        snprintf(cmdline_path, sizeof(cmdline_path), "/proc/%d/cmdline", pid);
        FILE *f = fopen(cmdline_path, "r");
        if (!f) continue;

        char cmdline[512];
        size_t n = fread(cmdline, 1, sizeof(cmdline) - 1, f);
        fclose(f);
        cmdline[n] = '\0';
        if (n == 0 || !strstr(cmdline, "termux-adb-bridge")) continue;

        if (kill(pid, SIGTERM) == 0) {
            for (int i = 0; i < 50; i++) {
                if (kill(pid, 0) != 0) break;
                usleep(10000);
            }
            if (kill(pid, 0) == 0) {
                kill(pid, SIGKILL);
                for (int i = 0; i < 50; i++) {
                    if (kill(pid, 0) != 0) break;
                    usleep(10000);
                }
            }
            fprintf(stderr, "Killed previous instance (PID %d)\n", pid);
        }
    }
    closedir(proc);
}

int server_start(server_t *sv) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    kill_previous_instance();

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(sv->port);
    inet_pton(AF_INET, sv->address, &addr.sin_addr);

    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(fd); return -1;
    }

    if (listen(fd, 128) < 0) {
        perror("listen"); close(fd); return -1;
    }

    printf("Listening on %s:%d\n", sv->address, sv->port);

    sv->running = 1;
    while (sv->running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(fd, (struct sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            if (!sv->running) break;
            perror("accept");
            continue;
        }

        client_job_t *job = malloc(sizeof(client_job_t));
        job->ssl_ctx = sv->ssl_ctx;
        job->fd = client_fd;

        pthread_t tid;
        pthread_create(&tid, NULL, handle_client, job);
        pthread_detach(tid);
    }

    close(fd);
    printf("Server stopped\n");
    return 0;
}

void server_stop(server_t *sv) {
    sv->running = 0;
}
