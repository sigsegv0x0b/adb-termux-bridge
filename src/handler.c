#include "handler.h"
#include "executor.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <openssl/ssl.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#define PACKAGE_VERSION "0.1.0"
#define MAX_ENV 256

static char g_cert_dir[1024] = "";
static char **g_argv = NULL;
static int g_listen_fd = -1;

void handler_set_cert_dir(const char *dir) {
    strncpy(g_cert_dir, dir, sizeof(g_cert_dir) - 1);
    g_cert_dir[sizeof(g_cert_dir) - 1] = '\0';
}

void handler_set_argv(char **argv) {
    g_argv = argv;
}

void handler_set_listen_fd(int fd) {
    g_listen_fd = fd;
}

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0) { fclose(f); return NULL; }
    char *data = malloc(len + 1);
    if (!data) { fclose(f); return NULL; }
    size_t n = fread(data, 1, len, f);
    fclose(f);
    data[n] = '\0';
    return data;
}

static char *compute_fingerprint(const char *cert_path) {
    FILE *f = fopen(cert_path, "rb");
    if (!f) return NULL;
    X509 *cert = PEM_read_X509(f, NULL, NULL, NULL);
    fclose(f);
    if (!cert) return NULL;

    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int mdlen;
    X509_digest(cert, EVP_sha256(), md, &mdlen);

    // Format as SHA256:xx:xx:xx...
    char *fp = malloc(mdlen * 3 + 8);
    if (!fp) { X509_free(cert); return NULL; }
    int pos = snprintf(fp, 8, "SHA256:");
    for (unsigned int i = 0; i < mdlen; i++) {
        pos += snprintf(fp + pos, 4, "%02x", md[i]);
        if (i < mdlen - 1) fp[pos++] = ':';
    }
    fp[pos] = '\0';
    X509_free(cert);
    return fp;
}

static int streq(const char *a, const char *b) {
    return strcmp(a, b) == 0;
}

static int startswith(const char *s, const char *prefix) {
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

void send_chunked_headers(SSL *ssl) {
    const char *h =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: keep-alive\r\n"
        "\r\n";
    SSL_write(ssl, h, strlen(h));
}

static void send_http_response(SSL *ssl, int status, const char *status_text,
                                const char *content_type, const char *body, size_t body_len) {
    char buf[4096];
    int n = snprintf(buf, sizeof(buf),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "\r\n",
        status, status_text, content_type, body_len);
    SSL_write(ssl, buf, n);
    if (body_len > 0) SSL_write(ssl, body, body_len);
}

static void send_json(SSL *ssl, int status, const char *body) {
    send_http_response(ssl, status,
        status == 200 ? "OK" :
        status == 400 ? "Bad Request" :
        status == 404 ? "Not Found" : "Internal Server Error",
        "application/json", body, strlen(body));
}

static void send_error(SSL *ssl, int status, const char *msg) {
    cJSON *e = cJSON_CreateObject();
    cJSON_AddStringToObject(e, "error", msg);
    char *s = cJSON_PrintUnformatted(e);
    send_json(ssl, status, s);
    free(s);
    cJSON_Delete(e);
}

void send_chunk(SSL *ssl, const char *data, size_t len) {
    char hdr[32];
    int hn = snprintf(hdr, sizeof(hdr), "%zx\r\n", len);
    SSL_write(ssl, hdr, hn);
    if (len > 0) SSL_write(ssl, data, len);
    SSL_write(ssl, "\r\n", 2);
}

void send_chunk_final(SSL *ssl) {
    SSL_write(ssl, "0\r\n\r\n", 5);
}

static int stream_callback(const char *data, size_t len, int is_stderr, void *userdata) {
    SSL *ssl = (SSL *)userdata;
    char buf[4096];
    int n = snprintf(buf, sizeof(buf), "data: %s%.*s\n\n", is_stderr ? "[STDERR] " : "", (int)len, data);
    int hn = snprintf(buf + n, sizeof(buf) - n, "%zx\r\n", (size_t)n);
    SSL_write(ssl, buf + n, hn);
    SSL_write(ssl, buf, n);
    SSL_write(ssl, "\r\n", 2);
    return 0;
}

static char **parse_env(cJSON *req, int *env_count) {
    *env_count = 0;
    cJSON *env_obj = cJSON_GetObjectItem(req, "env");
    if (!env_obj || !cJSON_IsObject(env_obj)) return NULL;

    char **env = malloc(sizeof(char *) * MAX_ENV);
    if (!env) return NULL;

    cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, env_obj) {
        if (*env_count >= MAX_ENV) break;
        if (!cJSON_IsString(entry) || !entry->string || !entry->valuestring) continue;

        size_t klen = strlen(entry->string);
        size_t vlen = strlen(entry->valuestring);
        env[*env_count] = malloc(klen + 1 + vlen + 1);
        if (!env[*env_count]) continue;
        memcpy(env[*env_count], entry->string, klen);
        env[*env_count][klen] = '=';
        memcpy(env[*env_count] + klen + 1, entry->valuestring, vlen);
        env[*env_count][klen + 1 + vlen] = '\0';
        (*env_count)++;
    }
    return env;
}

static void free_env(char **env, int env_count) {
    if (!env) return;
    for (int i = 0; i < env_count; i++) free(env[i]);
    free(env);
}

static void handle_exec(SSL *ssl, const char *body, size_t body_len) {
    (void)body_len;
    cJSON *req = cJSON_Parse(body);
    if (!req) { send_error(ssl, 400, "invalid JSON"); return; }

    cJSON *cmd_item = cJSON_GetObjectItem(req, "command");
    if (!cmd_item || !cJSON_IsString(cmd_item)) {
        cJSON_Delete(req);
        send_error(ssl, 400, "missing 'command' field");
        return;
    }

    int env_count = 0;
    char **env = parse_env(req, &env_count);

    exec_result_t result;
    if (exec_run(cmd_item->valuestring, env, env_count, &result) < 0) {
        free_env(env, env_count);
        cJSON_Delete(req);
        send_error(ssl, 500, "execution failed");
        return;
    }

    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "stdout", result.stdout_data ? result.stdout_data : "");
    cJSON_AddStringToObject(r, "stderr", result.stderr_data ? result.stderr_data : "");
    cJSON_AddNumberToObject(r, "exit_code", (double)result.exit_code);
    char *s = cJSON_PrintUnformatted(r);
    send_json(ssl, 200, s);
    free(s);
    cJSON_Delete(r);
    exec_free_result(&result);
    free_env(env, env_count);
    cJSON_Delete(req);
}

static void handle_exec_stream(SSL *ssl, const char *body, size_t body_len) {
    (void)body_len;
    cJSON *req = cJSON_Parse(body);
    if (!req) { send_error(ssl, 400, "invalid JSON"); return; }

    cJSON *cmd_item = cJSON_GetObjectItem(req, "command");
    if (!cmd_item || !cJSON_IsString(cmd_item)) {
        cJSON_Delete(req);
        send_error(ssl, 400, "missing 'command' field");
        return;
    }

    int env_count = 0;
    char **env = parse_env(req, &env_count);

    send_chunked_headers(ssl);

    int exit_code = exec_run_stream(cmd_item->valuestring, env, env_count, stream_callback, ssl);

    char final_msg[256];
    int n = snprintf(final_msg, sizeof(final_msg),
        "data: {\"event\":\"exit\",\"exit_code\":%d}\n\n", exit_code);
    send_chunk(ssl, final_msg, n);
    send_chunk_final(ssl);

    free_env(env, env_count);
    cJSON_Delete(req);
}

void handle_upload(SSL *ssl, const char *query, long content_length) {
    const char *path = NULL;
    if (startswith(query, "path=")) path = query + 5;

    if (!path || *path == '\0') {
        send_error(ssl, 400, "missing 'path' query parameter");
        return;
    }

    FILE *f = fopen(path, "wb");
    if (!f) {
        char err[512];
        snprintf(err, sizeof(err), "cannot open file: %s (%s)", path, strerror(errno));
        send_error(ssl, 500, err);
        return;
    }

    size_t written = 0;
    char chunk[65536];
    while (written < (size_t)content_length) {
        size_t remaining = (size_t)content_length - written;
        size_t to_read = remaining < sizeof(chunk) ? remaining : sizeof(chunk);
        int n = SSL_read(ssl, chunk, to_read);
        if (n <= 0) break;
        size_t w = fwrite(chunk, 1, n, f);
        written += w;
        if (w != (size_t)n) break;
    }
    fclose(f);

    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "bytes_written", (double)written);
    char *s = cJSON_PrintUnformatted(r);
    send_json(ssl, 200, s);
    free(s);
    cJSON_Delete(r);
}

int handle_update(SSL *ssl, const char *query, long content_length) {
    // Parse sha256 from query
    char expected_sha[128] = "";
    if (startswith(query, "sha256=")) {
        strncpy(expected_sha, query + 7, sizeof(expected_sha) - 1);
        expected_sha[sizeof(expected_sha) - 1] = '\0';
        char *amp = strchr(expected_sha, '&');
        if (amp) *amp = '\0';
    }

    if (strlen(expected_sha) != 64) {
        send_error(ssl, 400, "missing or invalid sha256 query parameter (need 64 hex chars)");
        return 0;
    }

    if (content_length <= 0) {
        send_error(ssl, 400, "missing body (Content-Length required)");
        return 1;
    }

    // Discover own binary path
    char binary_path[4096];
    ssize_t blen = readlink("/proc/self/exe", binary_path, sizeof(binary_path) - 1);
    if (blen <= 0) {
        send_error(ssl, 500, "cannot determine own binary path");
        return 0;
    }
    binary_path[blen] = '\0';

    // Write to temp file beside the binary (same filesystem for atomic rename)
    char tmp_path[4096];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", binary_path);

    FILE *f = fopen(tmp_path, "wb");
    if (!f) {
        char err[512];
        snprintf(err, sizeof(err), "cannot open temp file: %s (%s)", tmp_path, strerror(errno));
        send_error(ssl, 500, err);
        return 0;
    }

    // Stream body to temp file while computing SHA256
    EVP_MD_CTX *mdctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(mdctx, EVP_sha256(), NULL);

    size_t written = 0;
    char chunk[65536];
    while (written < (size_t)content_length) {
        size_t remaining = (size_t)content_length - written;
        size_t to_read = remaining < sizeof(chunk) ? remaining : sizeof(chunk);
        int n = SSL_read(ssl, chunk, to_read);
        if (n <= 0) break;
        size_t w = fwrite(chunk, 1, n, f);
        if (w != (size_t)n) break;
        EVP_DigestUpdate(mdctx, chunk, n);
        written += w;
    }

    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int md_len = 0;
    EVP_DigestFinal_ex(mdctx, md, &md_len);
    EVP_MD_CTX_free(mdctx);

    if (written != (size_t)content_length) {
        fclose(f);
        unlink(tmp_path);
        char err[128];
        snprintf(err, sizeof(err), "short read: got %zu of %ld bytes", written, content_length);
        send_error(ssl, 400, err);
        return 1;
    }

    // Convert computed hash to hex
    char computed_sha[65];
    for (unsigned int i = 0; i < md_len; i++)
        snprintf(computed_sha + i * 2, 3, "%02x", md[i]);
    computed_sha[64] = '\0';

    // Compare (case-insensitive)
    if (strcasecmp(computed_sha, expected_sha) != 0) {
        fclose(f);
        unlink(tmp_path);
        char err[256];
        snprintf(err, sizeof(err), "checksum mismatch: expected %s, got %s", expected_sha, computed_sha);
        send_error(ssl, 400, err);
        return 1;
    }

    // Checksum matches — atomically replace the binary
    fsync(fileno(f));
    fclose(f);
    chmod(tmp_path, 0755);

    if (rename(tmp_path, binary_path) < 0) {
        char err[512];
        snprintf(err, sizeof(err), "rename failed: %s (%s)", binary_path, strerror(errno));
        send_error(ssl, 500, err);
        return 1;
    }

    // Send success response
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "status", "ok");
    cJSON_AddStringToObject(r, "message", "restarting");
    cJSON_AddStringToObject(r, "sha256", computed_sha);
    char *s = cJSON_PrintUnformatted(r);
    send_json(ssl, 200, s);
    free(s);
    cJSON_Delete(r);

    // Flush SSL, close everything, execv the new binary
    SSL_shutdown(ssl);
    SSL_free(ssl);
    if (g_listen_fd >= 0) close(g_listen_fd);

    execv(binary_path, g_argv);

    // If execv returns, it failed
    perror("execv");
    exit(1);
}

void handle_exec_pipe(SSL *ssl, const char *query, long content_length, int chunked) {
    // Parse command from query
    const char *q = query;
    const char *cmd = NULL;
    size_t cmd_len = 0;
    char env_keys[MAX_ENV][256];
    char env_vals[MAX_ENV][256];
    int env_count = 0;
    int raw = 0;

    while (q && *q) {
        const char *amp = strchr(q, '&');
        size_t seg_len = amp ? (size_t)(amp - q) : strlen(q);

        if (strncmp(q, "command=", 8) == 0 && seg_len > 8) {
            cmd = q + 8;
            cmd_len = seg_len - 8;
        } else if (strncmp(q, "raw=1", 5) == 0 && seg_len == 5) {
            raw = 1;
        } else if (strncmp(q, "env_", 4) == 0 && env_count < MAX_ENV) {
            const char *eq = q + 4;
            const char *eq_end = memchr(eq, '=', seg_len - 4);
            if (eq_end) {
                size_t klen = eq_end - eq;
                size_t vlen = seg_len - 4 - klen - 1;
                if (klen < sizeof(env_keys[env_count]) && vlen < sizeof(env_vals[env_count])) {
                    memcpy(env_keys[env_count], eq, klen);
                    env_keys[env_count][klen] = '\0';
                    memcpy(env_vals[env_count], eq_end + 1, vlen);
                    env_vals[env_count][vlen] = '\0';
                    env_count++;
                }
            }
        }

        q = amp ? amp + 1 : NULL;
    }

    if (!cmd || cmd_len == 0) {
        send_error(ssl, 400, "missing 'command' query parameter");
        return;
    }

    // URL-decode command into a buffer
    char cmd_buf[4096];
    size_t ci = 0;
    for (size_t i = 0; i < cmd_len && ci < sizeof(cmd_buf) - 1; i++) {
        if (cmd[i] == '%' && i + 2 < cmd_len) {
            char hex[3] = { cmd[i+1], cmd[i+2], '\0' };
            cmd_buf[ci++] = (char)strtol(hex, NULL, 16);
            i += 2;
        } else if (cmd[i] == '+') {
            cmd_buf[ci++] = ' ';
        } else {
            cmd_buf[ci++] = cmd[i];
        }
    }
    cmd_buf[ci] = '\0';

    // Build env array
    char **env = NULL;
    if (env_count > 0) {
        env = malloc(sizeof(char *) * env_count);
        for (int i = 0; i < env_count; i++) {
            size_t klen = strlen(env_keys[i]);
            size_t vlen = strlen(env_vals[i]);
            env[i] = malloc(klen + 1 + vlen + 1);
            memcpy(env[i], env_keys[i], klen);
            env[i][klen] = '=';
            memcpy(env[i] + klen + 1, env_vals[i], vlen);
            env[i][klen + 1 + vlen] = '\0';
        }
    }

    // Send chunked response headers
    const char *hdr = raw ?
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/octet-stream\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: keep-alive\r\n"
        "\r\n"
        :
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: keep-alive\r\n"
        "\r\n";
    SSL_write(ssl, hdr, strlen(hdr));

    exec_run_pipe(cmd_buf, env, env_count, ssl, content_length, chunked, raw);

    for (int i = 0; i < env_count; i++) free(env[i]);
    free(env);
}

static void handle_download(SSL *ssl, const char *query) {
    const char *path = NULL;
    if (startswith(query, "path=")) path = query + 5;

    if (!path || *path == '\0') {
        send_error(ssl, 400, "missing 'path' query parameter");
        return;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        char err[512];
        snprintf(err, sizeof(err), "cannot open file: %s", path);
        send_error(ssl, 404, err);
        return;
    }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    if (fsize < 0) { fclose(f); send_error(ssl, 500, "cannot determine file size"); return; }
    if (fsize > 100 * 1024 * 1024) { fclose(f); send_error(ssl, 413, "file too large"); return; }
    fseek(f, 0, SEEK_SET);

    char *data = malloc(fsize + 1);
    if (!data) { fclose(f); send_error(ssl, 500, "out of memory"); return; }

    size_t nread = fread(data, 1, fsize, f);
    fclose(f);
    data[nread] = '\0';

    char hdr[4096];
    int hn = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/octet-stream\r\n"
        "Content-Length: %zu\r\n"
        "Content-Disposition: attachment; filename=\"%s\"\r\n"
        "Connection: close\r\n"
        "\r\n",
        nread, path);
    SSL_write(ssl, hdr, hn);
    SSL_write(ssl, data, nread);
    free(data);
}

static void handle_health(SSL *ssl) {
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "status", "ok");
    cJSON_AddStringToObject(r, "version", PACKAGE_VERSION);
    char *s = cJSON_PrintUnformatted(r);
    send_json(ssl, 200, s);
    free(s);
    cJSON_Delete(r);
}

static void handle_certinfo(SSL *ssl) {
    char ca_path[1024], cert_path[1024];
    snprintf(ca_path, sizeof(ca_path), "%s/ca.crt", g_cert_dir);
    snprintf(cert_path, sizeof(cert_path), "%s/client.crt", g_cert_dir);

    char *ca_pem = read_file(ca_path);
    char *client_pem = read_file(cert_path);
    char *fingerprint = compute_fingerprint(ca_path);

    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "ca_pem", ca_pem ? ca_pem : "");
    cJSON_AddStringToObject(r, "client_cert_pem", client_pem ? client_pem : "");
    cJSON_AddStringToObject(r, "fingerprint", fingerprint ? fingerprint : "");
    char *s = cJSON_PrintUnformatted(r);
    send_json(ssl, 200, s);
    free(s);
    cJSON_Delete(r);

    free(ca_pem);
    free(client_pem);
    free(fingerprint);
}

int handle_request(SSL *ssl, const char *method, const char *path,
                   const char *query, const char *body, size_t body_len) {
    if (streq(path, "/api/health") && streq(method, "GET")) {
        handle_health(ssl);
        return 1;
    }

    if (streq(path, "/api/certinfo") && streq(method, "GET")) {
        handle_certinfo(ssl);
        return 1;
    }

    if (streq(path, "/api/exec") && streq(method, "POST")) {
        handle_exec(ssl, body, body_len);
        return 1;
    }

    if (streq(path, "/api/exec/stream") && streq(method, "POST")) {
        handle_exec_stream(ssl, body, body_len);
        return 1;
    }

    if (streq(path, "/api/upload") && streq(method, "POST")) {
        return 0; // handled in server.c with streaming
    }

    if (streq(path, "/api/download") && streq(method, "GET")) {
        handle_download(ssl, query);
        return 1;
    }

    return 0;
}
