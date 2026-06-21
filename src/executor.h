#ifndef EXECUTOR_H
#define EXECUTOR_H

#include <stddef.h>
#include <openssl/ssl.h>

#define EXEC_MAX_OUTPUT (10 * 1024 * 1024)

typedef struct {
    int   exit_code;
    char *stdout_data;
    char *stderr_data;
    size_t stdout_len;
    size_t stderr_len;
} exec_result_t;

int exec_run(const char *command, char **env, int env_count, exec_result_t *result);
void exec_free_result(exec_result_t *result);

typedef int (*exec_output_fn)(const char *data, size_t len, int is_stderr, void *userdata);

int exec_run_stream(const char *command, char **env, int env_count, exec_output_fn fn, void *userdata);

int exec_run_pipe(const char *command, char **env, int env_count, SSL *ssl,
                  long content_length, int chunked, int raw);

#endif
