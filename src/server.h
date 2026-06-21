#ifndef SERVER_H
#define SERVER_H

#include <openssl/ssl.h>

typedef struct {
    SSL_CTX *ssl_ctx;
    char     address[64];
    int      port;
    int      listen_fd;
    volatile int running;
} server_t;

int server_start(server_t *sv);
void server_stop(server_t *sv);

#endif
