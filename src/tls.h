#ifndef TLS_H
#define TLS_H

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <stdio.h>

typedef struct {
    char ca_cert[1024];
    char ca_key[1024];
    char server_cert[1024];
    char server_key[1024];
    char client_cert[1024];
    char client_key[1024];
} cert_paths_t;

int  tls_init(void);
int  tls_generate_certs(const char *cert_dir);
void tls_build_paths(cert_paths_t *paths, const char *cert_dir);
SSL_CTX *tls_create_server_ctx(const cert_paths_t *paths, X509 **out_ca, X509 **out_cert);

int tls_print_cert(const char *cert_dir, FILE *out);
int tls_print_ca(const char *cert_dir, FILE *out);
int tls_print_fingerprint(const char *cert_dir, FILE *out);
int tls_print_pubkey_hex(const char *cert_dir, FILE *out);
int tls_embed_c_header(const char *cert_dir, const char *output_path);
int tls_save_to_fingerprint_dir(const char *cert_dir);

#endif
