#include "executor.h"
#include "handler.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <errno.h>
#include <poll.h>
#include <openssl/err.h>

static void apply_env(char **env, int env_count) {
    for (int i = 0; i < env_count; i++) {
        char *eq = strchr(env[i], '=');
        if (!eq) continue;
        *eq = '\0';
        setenv(env[i], eq + 1, 1);
        *eq = '=';
    }
}

int exec_run(const char *command, char **env, int env_count, exec_result_t *result) {
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
        apply_env(env, env_count);
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

int exec_run_stream(const char *command, char **env, int env_count, exec_output_fn fn, void *userdata) {
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
        apply_env(env, env_count);
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

int exec_run_pipe(const char *command, char **env, int env_count, SSL *ssl,
                  long content_length, int chunked, int raw) {
    int stdin_pipe[2], out_pipe[2], err_pipe[2];
    if (pipe(stdin_pipe) < 0 || pipe(out_pipe) < 0 || pipe(err_pipe) < 0) {
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(stdin_pipe[0]); close(stdin_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        close(err_pipe[0]); close(err_pipe[1]);
        return -1;
    }

    if (pid == 0) {
        close(stdin_pipe[1]);
        close(out_pipe[0]);
        close(err_pipe[0]);
        dup2(stdin_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(err_pipe[1], STDERR_FILENO);
        close(stdin_pipe[0]);
        close(out_pipe[1]);
        close(err_pipe[1]);
        apply_env(env, env_count);
        execl("/system/bin/sh", "sh", "-c", command, (char*)NULL);
        _exit(127);
    }

    close(stdin_pipe[0]);
    close(out_pipe[1]);
    close(err_pipe[1]);

    // Non-blocking mode for all fds
    int ssl_fd = SSL_get_fd(ssl);
    int ssl_flags = fcntl(ssl_fd, F_GETFL, 0);
    fcntl(ssl_fd, F_SETFL, ssl_flags | O_NONBLOCK);
    fcntl(stdin_pipe[1], F_SETFL, fcntl(stdin_pipe[1], F_GETFL, 0) | O_NONBLOCK);
    fcntl(out_pipe[0], F_SETFL, fcntl(out_pipe[0], F_GETFL, 0) | O_NONBLOCK);
    fcntl(err_pipe[0], F_SETFL, fcntl(err_pipe[0], F_GETFL, 0) | O_NONBLOCK);

    // Body reader state machine
    enum { BR_HEADER, BR_DATA, BR_TRAILER, BR_FINAL, BR_DONE } br_state;
    long br_remaining;
    char br_hdr[32];
    int br_hdr_len = 0;

    if (chunked) {
        br_state = BR_HEADER;
        br_remaining = -1;
    } else if (content_length > 0) {
        br_state = BR_DATA;
        br_remaining = content_length;
    } else {
        br_state = BR_DONE;
    }

    int body_eof = (br_state == BR_DONE);
    int stdin_closed = (br_state == BR_DONE);
    int stdout_done = 0, stderr_done = 0;
    int child_exited = 0;
    int exit_code = -1;
    int ssl_error = 0;

    char stdin_buf[65536];
    size_t stdin_buf_len = 0;

    char sse_buf[131072];
    size_t sse_buf_len = 0;

    char out_buf[65536];

    int ssl_want_read = 0;
    int ssl_want_write = 0;

    if (stdin_closed) {
        close(stdin_pipe[1]);
        stdin_pipe[1] = -1;
    }

    while (!ssl_error &&
           (!child_exited || !stdin_closed || !stdout_done || !stderr_done || sse_buf_len > 0)) {
        // If SSL has buffered data, don't block on poll — we need to read
        // that data and write it to the child before waiting for anything
        int poll_timeout = (!body_eof && SSL_pending(ssl) > 0 &&
                            stdin_buf_len < sizeof(stdin_buf)) ? 0 : -1;

        struct pollfd fds[5];
        int nfds = 0;
        int ssl_idx = -1;

        int need_read = !body_eof && stdin_buf_len < sizeof(stdin_buf);
        int need_write = sse_buf_len > 0;

        if (need_read || need_write || ssl_want_read || ssl_want_write || SSL_pending(ssl) > 0) {
            fds[nfds].fd = ssl_fd;
            fds[nfds].events = 0;
            if (need_read || ssl_want_read || SSL_pending(ssl) > 0) fds[nfds].events |= POLLIN;
            if (need_write || ssl_want_write) fds[nfds].events |= POLLOUT;
            ssl_idx = nfds;
            nfds++;
        }

        if (!stdin_closed && stdin_buf_len > 0) {
            fds[nfds].fd = stdin_pipe[1];
            fds[nfds].events = POLLOUT;
            nfds++;
        }

        if (!stdout_done) {
            fds[nfds].fd = out_pipe[0];
            fds[nfds].events = POLLIN;
            nfds++;
        }

        if (!stderr_done) {
            fds[nfds].fd = err_pipe[0];
            fds[nfds].events = POLLIN;
            nfds++;
        }

        if (nfds == 0) {
            // Nothing to poll but child might still be running — wait for it
            if (!child_exited) {
                int wstatus;
                if (waitpid(pid, &wstatus, 0) == pid) {
                    child_exited = 1;
                    exit_code = WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) :
                                WIFSIGNALED(wstatus) ? -WTERMSIG(wstatus) : -1;
                }
            }
            break;
        }

        int ret = poll(fds, nfds, poll_timeout);
        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }

        // SSL read — body data from client (also handles SSL_pending)
        if (ssl_idx >= 0 && ((fds[ssl_idx].revents & (POLLIN | POLLHUP)) || SSL_pending(ssl) > 0)) {
            ssl_want_read = 0;
            size_t space = sizeof(stdin_buf) - stdin_buf_len;

            if (br_state == BR_HEADER) {
                char c;
                int n = SSL_read(ssl, &c, 1);
                if (n == 1) {
                    if (c == '\n') {
                        br_hdr[br_hdr_len] = '\0';
                        br_remaining = strtol(br_hdr, NULL, 16);
                        br_hdr_len = 0;
                        br_state = (br_remaining == 0) ? BR_FINAL : BR_DATA;
                    } else if (br_hdr_len < (int)sizeof(br_hdr) - 1) {
                        br_hdr[br_hdr_len++] = c;
                    }
                } else {
                    int err = SSL_get_error(ssl, n);
                    if (err == SSL_ERROR_WANT_READ) ssl_want_read = 1;
                    else if (err == SSL_ERROR_WANT_WRITE) ssl_want_write = 1;
                    else { body_eof = 1; br_state = BR_DONE; }
                }
            } else if (br_state == BR_DATA && space > 0) {
                long to_read = br_remaining < (long)space ? br_remaining : (long)space;
                int n = SSL_read(ssl, stdin_buf + stdin_buf_len, (int)to_read);
                if (n > 0) {
                    stdin_buf_len += n;
                    br_remaining -= n;
                    if (br_remaining == 0) {
                        br_state = chunked ? BR_TRAILER : BR_DONE;
                        if (br_state == BR_DONE) body_eof = 1;
                    }
                } else {
                    int err = SSL_get_error(ssl, n);
                    if (err == SSL_ERROR_WANT_READ) ssl_want_read = 1;
                    else if (err == SSL_ERROR_WANT_WRITE) ssl_want_write = 1;
                    else { body_eof = 1; br_state = BR_DONE; }
                }
            } else if (br_state == BR_TRAILER || br_state == BR_FINAL) {
                char c;
                int n = SSL_read(ssl, &c, 1);
                if (n == 1) {
                    if (c == '\n') {
                        if (br_state == BR_TRAILER) br_state = BR_HEADER;
                        else { br_state = BR_DONE; body_eof = 1; }
                    }
                } else {
                    int err = SSL_get_error(ssl, n);
                    if (err == SSL_ERROR_WANT_READ) ssl_want_read = 1;
                    else if (err == SSL_ERROR_WANT_WRITE) ssl_want_write = 1;
                    else { body_eof = 1; br_state = BR_DONE; }
                }
            }
        }

        // SSL write — SSE output to client
        if (ssl_idx >= 0 && (fds[ssl_idx].revents & POLLOUT) && sse_buf_len > 0) {
            ssl_want_write = 0;
            int n = SSL_write(ssl, sse_buf, (int)sse_buf_len);
            if (n > 0) {
                memmove(sse_buf, sse_buf + n, sse_buf_len - n);
                sse_buf_len -= n;
            } else {
                int err = SSL_get_error(ssl, n);
                if (err == SSL_ERROR_WANT_WRITE) ssl_want_write = 1;
                else { ssl_error = 1; break; }
            }
        }

        // stdin_pipe write — push body data to child
        if (!stdin_closed && stdin_buf_len > 0) {
            for (int i = 0; i < nfds; i++) {
                if (fds[i].fd == stdin_pipe[1] && (fds[i].revents & POLLOUT)) {
                    ssize_t n = write(stdin_pipe[1], stdin_buf, stdin_buf_len);
                    if (n > 0) {
                        if ((size_t)n < stdin_buf_len)
                            memmove(stdin_buf, stdin_buf + n, stdin_buf_len - n);
                        stdin_buf_len -= n;
                    } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                        close(stdin_pipe[1]);
                        stdin_pipe[1] = -1;
                        stdin_closed = 1;
                    }
                    break;
                }
            }
        }

        // stdout/stderr read — child output
        for (int i = 0; i < nfds; i++) {
            if (fds[i].revents & (POLLIN | POLLHUP)) {
                int is_err = 0;
                int fd = -1;
                if (fds[i].fd == out_pipe[0]) { fd = out_pipe[0]; is_err = 0; }
                else if (fds[i].fd == err_pipe[0]) { fd = err_pipe[0]; is_err = 1; }

                if (fd >= 0) {
                    ssize_t n = read(fd, out_buf, sizeof(out_buf));
                    if (n > 0) {
                        if (raw) {
                            // Raw mode: frame stdout bytes as a chunked transfer chunk
                            // Discard stderr (raw mode is for binary piping)
                            if (!is_err) {
                                char chunk_hdr[16];
                                int hn = snprintf(chunk_hdr, sizeof(chunk_hdr),
                                    "%zx\r\n", (size_t)n);
                                if (sse_buf_len + hn + n + 2 <= sizeof(sse_buf)) {
                                    memcpy(sse_buf + sse_buf_len, chunk_hdr, hn);
                                    sse_buf_len += hn;
                                    memcpy(sse_buf + sse_buf_len, out_buf, n);
                                    sse_buf_len += n;
                                    sse_buf[sse_buf_len++] = '\r';
                                    sse_buf[sse_buf_len++] = '\n';
                                }
                            }
                        } else {
                            // SSE mode: wrap each line in data: ...\n\n
                            out_buf[n] = '\0';
                            char *line = out_buf;
                            while (line && sse_buf_len < sizeof(sse_buf) - 512) {
                                char *nl = strchr(line, '\n');
                                if (nl) *nl = '\0';
                                if (*line) {
                                    int llen = (int)strlen(line);
                                    int data_len = 6 + (is_err ? 9 : 0) + llen + 2;
                                    int pos = 0;
                                    pos += snprintf((char *)sse_buf + sse_buf_len, sizeof(sse_buf) - sse_buf_len,
                                        "%x\r\n", data_len);
                                    pos += snprintf((char *)sse_buf + sse_buf_len + pos, sizeof(sse_buf) - sse_buf_len - pos,
                                        "data: %s%s\n\n\r\n", is_err ? "[STDERR] " : "", line);
                                    sse_buf_len += pos;
                                }
                                line = nl ? nl + 1 : NULL;
                            }
                        }
                    } else {
                        if (is_err) stderr_done = 1; else stdout_done = 1;
                    }
                }
            }
        }

        // Close stdin when body is fully read and buffer drained
        if (body_eof && stdin_buf_len == 0 && !stdin_closed) {
            close(stdin_pipe[1]);
            stdin_pipe[1] = -1;
            stdin_closed = 1;
        }

        // Check child exit
        if (!child_exited) {
            int wstatus;
            if (waitpid(pid, &wstatus, WNOHANG) == pid) {
                child_exited = 1;
                exit_code = WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) :
                            WIFSIGNALED(wstatus) ? -WTERMSIG(wstatus) : -1;
            }
        }
    }

    // Kill and reap child if still running (e.g., SSL error)
    if (!child_exited) {
        kill(pid, SIGTERM);
        int wstatus;
        waitpid(pid, &wstatus, 0);
        exit_code = -1;
    }

    // Flush remaining output in blocking mode
    fcntl(ssl_fd, F_SETFL, ssl_flags);
    if (!ssl_error && sse_buf_len > 0) {
        SSL_write(ssl, sse_buf, (int)sse_buf_len);
        sse_buf_len = 0;
    }

    // Send termination
    if (!ssl_error) {
        if (raw) {
            // Raw mode: just send the final chunk
            send_chunk_final(ssl);
        } else {
            // SSE mode: send exit event + final chunk
            char exit_msg[256];
            int en = snprintf(exit_msg, sizeof(exit_msg),
                "data: {\"event\":\"exit\",\"exit_code\":%d}\n\n", exit_code);
            send_chunk(ssl, exit_msg, (size_t)en);
            send_chunk_final(ssl);
        }
    }

    if (stdin_pipe[1] >= 0) close(stdin_pipe[1]);
    close(out_pipe[0]);
    close(err_pipe[0]);

    return exit_code;
}
