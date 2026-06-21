#ifndef FORWARD_H
#define FORWARD_H

#include <openssl/ssl.h>

typedef struct tcpforward tcpforward_t;

int  forward_add(const char *unix_path, const char *host, int port, int secure);
int  forward_remove(int id);
void forward_list_json(char *buf, size_t size);
void forward_shutdown_all(void);

#endif
