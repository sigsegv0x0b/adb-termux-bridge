#ifndef HANDLER_H
#define HANDLER_H

#include <openssl/ssl.h>

void handler_set_cert_dir(const char *dir);
void handler_set_certs(X509 *ca, X509 *server);
void handler_set_argv(char **argv);
void handler_set_listen_fd(int fd);

int handle_request(SSL *ssl, const char *method, const char *path,
                   const char *query, const char *body, size_t body_len);

void handle_upload(SSL *ssl, const char *query, long content_length);
void handle_exec_pipe(SSL *ssl, const char *query, long content_length, int chunked);
int handle_update(SSL *ssl, const char *query, long content_length);
void handle_tcpforward_get(SSL *ssl);
void handle_tcpforward_post(SSL *ssl, const char *body, size_t body_len);
void handle_tcpforward_delete(SSL *ssl, const char *query);
void send_chunked_headers(SSL *ssl);
void send_chunk(SSL *ssl, const char *data, size_t len);
void send_chunk_final(SSL *ssl);

#endif
