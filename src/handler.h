#ifndef HANDLER_H
#define HANDLER_H

#include <openssl/ssl.h>

void handler_set_cert_dir(const char *dir);

int handle_request(SSL *ssl, const char *method, const char *path,
                   const char *query, const char *body, size_t body_len);

void handle_upload(SSL *ssl, const char *query, long content_length);
void handle_exec_pipe(SSL *ssl, const char *query, long content_length, int chunked);
void send_chunked_headers(SSL *ssl);
void send_chunk(SSL *ssl, const char *data, size_t len);
void send_chunk_final(SSL *ssl);

#endif
