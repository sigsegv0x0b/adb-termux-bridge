#include "handler.h"
#include "executor.h"
#include "json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <openssl/ssl.h>

#define PACKAGE_VERSION "0.1.0"

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
    char json[1024];
    snprintf(json, sizeof(json), "{\"error\":\"%s\"}", msg);
    send_json(ssl, status, json);
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

static void handle_exec(SSL *ssl, const char *body, size_t body_len) {
    (void)body_len;
    json_value_t *req = json_parse(body);
    if (!req) { send_error(ssl, 400, "invalid JSON"); return; }

    const char *cmd = json_get_string(req, "command");
    if (!cmd) { json_free(req); send_error(ssl, 400, "missing 'command' field"); return; }

    exec_result_t result;
    if (exec_run(cmd, &result) < 0) {
        json_free(req);
        send_error(ssl, 500, "execution failed");
        return;
    }

    json_value_t *r = json_new_object();
    json_add_string(r, "stdout", result.stdout_data ? result.stdout_data : "");
    json_add_string(r, "stderr", result.stderr_data ? result.stderr_data : "");
    json_add_number(r, "exit_code", result.exit_code);
    char *s = json_serialize(r);
    send_json(ssl, 200, s);
    free(s);
    json_free(r);
    exec_free_result(&result);
    json_free(req);
}

static void handle_exec_stream(SSL *ssl, const char *body, size_t body_len) {
    (void)body_len;
    json_value_t *req = json_parse(body);
    if (!req) { send_error(ssl, 400, "invalid JSON"); return; }

    const char *cmd = json_get_string(req, "command");
    if (!cmd) { json_free(req); send_error(ssl, 400, "missing 'command' field"); return; }

    send_chunked_headers(ssl);

    int exit_code = exec_run_stream(cmd, stream_callback, ssl);

    char final_msg[256];
    int n = snprintf(final_msg, sizeof(final_msg),
        "data: {\"event\":\"exit\",\"exit_code\":%d}\n\n", exit_code);
    send_chunk(ssl, final_msg, n);
    send_chunk_final(ssl);

    json_free(req);
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

    json_value_t *r = json_new_object();
    json_add_number(r, "bytes_written", written);
    char *s = json_serialize(r);
    send_json(ssl, 200, s);
    free(s);
    json_free(r);
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
    json_value_t *r = json_new_object();
    json_add_string(r, "status", "ok");
    json_add_string(r, "version", PACKAGE_VERSION);
    char *s = json_serialize(r);
    send_json(ssl, 200, s);
    free(s);
    json_free(r);
}

int handle_request(SSL *ssl, const char *method, const char *path,
                   const char *query, const char *body, size_t body_len) {
    if (streq(path, "/api/health") && streq(method, "GET")) {
        handle_health(ssl);
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
