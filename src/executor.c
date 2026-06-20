#include "executor.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <errno.h>
#include <poll.h>

int exec_run(const char *command, exec_result_t *result) {
    memset(result, 0, sizeof(*result));

    int out_pipe[2], err_pipe[2];
    if (pipe(out_pipe) < 0 || pipe(err_pipe) < 0) return -1;

    pid_t pid = fork();
    if (pid < 0) { close(out_pipe[0]); close(out_pipe[1]); close(err_pipe[0]); close(err_pipe[1]); return -1; }

    if (pid == 0) {
        close(out_pipe[0]);
        close(err_pipe[0]);
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(err_pipe[1], STDERR_FILENO);
        close(out_pipe[1]);
        close(err_pipe[1]);
        execl("/system/bin/sh", "sh", "-c", command, (char*)NULL);
        _exit(127);
    }

    close(out_pipe[1]);
    close(err_pipe[1]);

    char buf[65536];
    ssize_t n;

    while (1) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(out_pipe[0], &rfds);
        FD_SET(err_pipe[0], &rfds);
        int maxfd = (out_pipe[0] > err_pipe[0]) ? out_pipe[0] : err_pipe[0];

        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        int ret = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (ret < 0) break;
        if (ret == 0) {
            int w;
            if (waitpid(pid, &w, WNOHANG) == pid) {
                result->exit_code = WIFEXITED(w) ? WEXITSTATUS(w) : -1;
                break;
            }
            continue;
        }

        if (FD_ISSET(out_pipe[0], &rfds)) {
            n = read(out_pipe[0], buf, sizeof(buf));
            if (n > 0) {
                if (result->stdout_len + (size_t)n > EXEC_MAX_OUTPUT) n = EXEC_MAX_OUTPUT - result->stdout_len;
                if (n > 0) {
                    result->stdout_data = realloc(result->stdout_data, result->stdout_len + n + 1);
                    memcpy(result->stdout_data + result->stdout_len, buf, n);
                    result->stdout_len += n;
                    result->stdout_data[result->stdout_len] = '\0';
                }
            }
        }
        if (FD_ISSET(err_pipe[0], &rfds)) {
            n = read(err_pipe[0], buf, sizeof(buf));
            if (n > 0) {
                if (result->stderr_len + (size_t)n > EXEC_MAX_OUTPUT) n = EXEC_MAX_OUTPUT - result->stderr_len;
                if (n > 0) {
                    result->stderr_data = realloc(result->stderr_data, result->stderr_len + n + 1);
                    memcpy(result->stderr_data + result->stderr_len, buf, n);
                    result->stderr_len += n;
                    result->stderr_data[result->stderr_len] = '\0';
                }
            }
        }

        int w;
        if (waitpid(pid, &w, WNOHANG) == pid) {
            result->exit_code = WIFEXITED(w) ? WEXITSTATUS(w) : -1;
            break;
        }
    }

    close(out_pipe[0]);
    close(err_pipe[0]);
    return 0;
}

void exec_free_result(exec_result_t *result) {
    free(result->stdout_data);
    free(result->stderr_data);
    memset(result, 0, sizeof(*result));
}

int exec_run_stream(const char *command, exec_output_fn fn, void *userdata) {
    int out_pipe[2], err_pipe[2];
    if (pipe(out_pipe) < 0 || pipe(err_pipe) < 0) return -1;

    pid_t pid = fork();
    if (pid < 0) { close(out_pipe[0]); close(out_pipe[1]); close(err_pipe[0]); close(err_pipe[1]); return -1; }

    if (pid == 0) {
        close(out_pipe[0]);
        close(err_pipe[0]);
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(err_pipe[1], STDERR_FILENO);
        close(out_pipe[1]);
        close(err_pipe[1]);
        execl("/system/bin/sh", "sh", "-c", command, (char*)NULL);
        _exit(127);
    }

    close(out_pipe[1]);
    close(err_pipe[1]);

    char buf[65536];
    int exit_code = -1;

    while (1) {
        struct pollfd fds[2] = {
            { .fd = out_pipe[0], .events = POLLIN },
            { .fd = err_pipe[0], .events = POLLIN }
        };

        int ret = poll(fds, 2, 500);
        if (ret < 0) break;

        int data_read = 0;

        for (int i = 0; i < 2; i++) {
            if (fds[i].revents & POLLIN) {
                ssize_t n = read(fds[i].fd, buf, sizeof(buf));
                if (n > 0) {
                    data_read = 1;
                    if (fn(buf, n, i == 1, userdata) != 0) {
                        kill(pid, SIGTERM);
                        goto stream_done;
                    }
                }
            }
        }

        if (data_read) continue;

        int wstatus;
        pid_t wp = waitpid(pid, &wstatus, WNOHANG);
        if (wp == pid) {
            exit_code = WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) :
                        WIFSIGNALED(wstatus) ? -WTERMSIG(wstatus) : -1;
            break;
        }
        if (wp < 0) break;
    }

stream_done:
    close(out_pipe[0]);
    close(err_pipe[0]);
    return exit_code;
}
