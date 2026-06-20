#include "tls.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <openssl/evp.h>
#include <openssl/bn.h>
#include <openssl/asn1.h>

#ifdef SECURE_BUILD
#  include "certs_data.h"
#endif

static int mkdir_p(const char *dir) {
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", dir);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0700);
            *p = '/';
        }
    }
    return mkdir(tmp, 0700);
}

int tls_init(void) {
    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();
    return 1;
}

static EVP_PKEY *generate_ed25519_key(void) {
    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, NULL);
    if (!ctx) return NULL;
    EVP_PKEY *pkey = NULL;
    if (EVP_PKEY_keygen_init(ctx) <= 0 || EVP_PKEY_keygen(ctx, &pkey) <= 0) {
        EVP_PKEY_CTX_free(ctx);
        return NULL;
    }
    EVP_PKEY_CTX_free(ctx);
    return pkey;
}

static X509 *make_cert(EVP_PKEY *subject_key, EVP_PKEY *ca_key,
                       X509 *ca_cert, const char *cn, int is_ca, int serial) {
    X509 *x = X509_new();
    if (!x) return NULL;

    ASN1_INTEGER_set(X509_get_serialNumber(x), serial);

    X509_gmtime_adj(X509_get_notBefore(x), 0);
    X509_gmtime_adj(X509_get_notAfter(x), 3650L * 86400L);

    X509_set_pubkey(x, subject_key);

    X509_NAME *name = X509_get_subject_name(x);
    X509_NAME_add_entry_by_txt(name, "O", MBSTRING_ASC, (unsigned char*)"Termux ADB Bridge", -1, -1, 0);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, (unsigned char*)cn, -1, -1, 0);

    if (ca_cert) {
        X509_set_issuer_name(x, X509_get_subject_name(ca_cert));
    } else {
        X509_set_issuer_name(x, name);
    }

    if (is_ca) {
        X509V3_CTX ctx;
        X509V3_set_ctx_nodb(&ctx);
        X509V3_set_ctx(&ctx, ca_cert ? ca_cert : x, x, NULL, NULL, 0);
        X509_EXTENSION *ext = X509V3_EXT_conf_nid(NULL, &ctx, NID_basic_constraints, "critical,CA:TRUE");
        if (ext) X509_add_ext(x, ext, -1);
        X509_EXTENSION_free(ext);
    }

    if (ca_key) {
        X509_sign(x, ca_key, NULL);
    } else {
        X509_sign(x, subject_key, NULL);
    }

    return x;
}

static int write_pem(const char *path, const char *type_, void *data, int is_key) {
    (void)type_;
    FILE *f = fopen(path, "w");
    if (!f) { perror(path); return -1; }
    int r;
    if (is_key)
        r = PEM_write_PrivateKey(f, (EVP_PKEY*)data, NULL, NULL, 0, NULL, NULL);
    else
        r = PEM_write_X509(f, (X509*)data);
    if (r <= 0) ERR_print_errors_fp(stderr);
    fclose(f);
    chmod(path, is_key ? 0600 : 0644);
    return r;
}

int tls_generate_certs(const char *cert_dir) {
    mkdir_p(cert_dir);

    char ca_key_path[1024], ca_cert_path[1024];
    char srv_key_path[1024], srv_cert_path[1024];
    char cli_key_path[1024], cli_cert_path[1024];

    snprintf(ca_key_path, sizeof(ca_key_path), "%s/ca.key", cert_dir);
    snprintf(ca_cert_path, sizeof(ca_cert_path), "%s/ca.crt", cert_dir);
    snprintf(srv_key_path, sizeof(srv_key_path), "%s/server.key", cert_dir);
    snprintf(srv_cert_path, sizeof(srv_cert_path), "%s/server.crt", cert_dir);
    snprintf(cli_key_path, sizeof(cli_key_path), "%s/client.key", cert_dir);
    snprintf(cli_cert_path, sizeof(cli_cert_path), "%s/client.crt", cert_dir);

    EVP_PKEY *ca_key = generate_ed25519_key();
    if (!ca_key) { fprintf(stderr, "Failed to generate CA key\n"); return -1; }

    X509 *ca_cert = make_cert(ca_key, NULL, NULL, "Bridge CA", 1, 1);
    if (!ca_cert) { fprintf(stderr, "Failed to create CA cert\n"); EVP_PKEY_free(ca_key); return -1; }

    EVP_PKEY *srv_key = generate_ed25519_key();
    if (!srv_key) { fprintf(stderr, "Failed to generate server key\n"); return -1; }

    X509 *srv_cert = make_cert(srv_key, ca_key, ca_cert, "127.0.0.1", 0, 2);
    if (!srv_cert) { fprintf(stderr, "Failed to create server cert\n"); return -1; }

    EVP_PKEY *cli_key = generate_ed25519_key();
    if (!cli_key) { fprintf(stderr, "Failed to generate client key\n"); return -1; }

    X509 *cli_cert = make_cert(cli_key, ca_key, ca_cert, "client", 0, 3);
    if (!cli_cert) { fprintf(stderr, "Failed to create client cert\n"); return -1; }

    int ok = 1;
    if (write_pem(ca_key_path,  "PRIVATE KEY", ca_key,  1) <= 0) { fprintf(stderr, "Failed to write %s\n", ca_key_path);  ok = 0; }
    if (write_pem(ca_cert_path, "CERTIFICATE", ca_cert, 0) <= 0) { fprintf(stderr, "Failed to write %s\n", ca_cert_path); ok = 0; }
    if (write_pem(srv_key_path,  "PRIVATE KEY", srv_key,  1) <= 0) { fprintf(stderr, "Failed to write %s\n", srv_key_path);  ok = 0; }
    if (write_pem(srv_cert_path, "CERTIFICATE", srv_cert, 0) <= 0) { fprintf(stderr, "Failed to write %s\n", srv_cert_path); ok = 0; }
    if (write_pem(cli_key_path,  "PRIVATE KEY", cli_key,  1) <= 0) { fprintf(stderr, "Failed to write %s\n", cli_key_path);  ok = 0; }
    if (write_pem(cli_cert_path, "CERTIFICATE", cli_cert, 0) <= 0) { fprintf(stderr, "Failed to write %s\n", cli_cert_path); ok = 0; }

    EVP_PKEY_free(ca_key); X509_free(ca_cert);
    EVP_PKEY_free(srv_key); X509_free(srv_cert);
    EVP_PKEY_free(cli_key); X509_free(cli_cert);

    if (ok) {
        printf("Certificates generated in %s\n", cert_dir);
        return 0;
    }
    return -1;
}

void tls_build_paths(cert_paths_t *paths, const char *cert_dir) {
    snprintf(paths->ca_cert,     sizeof(paths->ca_cert),     "%s/ca.crt",     cert_dir);
    snprintf(paths->ca_key,      sizeof(paths->ca_key),      "%s/ca.key",     cert_dir);
    snprintf(paths->server_cert, sizeof(paths->server_cert), "%s/server.crt", cert_dir);
    snprintf(paths->server_key,  sizeof(paths->server_key),  "%s/server.key", cert_dir);
    snprintf(paths->client_cert, sizeof(paths->client_cert), "%s/client.crt", cert_dir);
    snprintf(paths->client_key,  sizeof(paths->client_key),  "%s/client.key", cert_dir);
}

static X509 *pem_file_to_x509(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    X509 *x = PEM_read_X509(f, NULL, NULL, NULL);
    fclose(f);
    return x;
}

static EVP_PKEY *pem_file_to_pkey(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    EVP_PKEY *k = PEM_read_PrivateKey(f, NULL, NULL, NULL);
    fclose(f);
    return k;
}

#ifdef SECURE_BUILD
static X509 *der_to_x509(const unsigned char *der, size_t len) {
    BIO *b = BIO_new_mem_buf(der, (int)len);
    if (!b) return NULL;
    X509 *x = d2i_X509_bio(b, NULL);
    BIO_free(b);
    return x;
}

static EVP_PKEY *der_to_pkey(const unsigned char *der, size_t len) {
    BIO *b = BIO_new_mem_buf(der, (int)len);
    if (!b) return NULL;
    EVP_PKEY *k = d2i_PrivateKey_bio(b, NULL);
    BIO_free(b);
    return k;
}

static X509 *load_server_cert(const char *path) {
    (void)path;
    return der_to_x509(server_crt, server_crt_len);
}
static X509 *load_ca_cert(const char *path) {
    (void)path;
    return der_to_x509(ca_crt, ca_crt_len);
}
static EVP_PKEY *load_server_key(const char *path) {
    (void)path;
    return der_to_pkey(server_key, server_key_len);
}
#else
static X509 *load_server_cert(const char *path) {
    return pem_file_to_x509(path);
}
static X509 *load_ca_cert(const char *path) {
    return pem_file_to_x509(path);
}
static EVP_PKEY *load_server_key(const char *path) {
    return pem_file_to_pkey(path);
}
#endif

SSL_CTX *tls_create_server_ctx(const cert_paths_t *paths) {
    SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx) {
        fprintf(stderr, "SSL_CTX_new failed\n");
        return NULL;
    }

    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    SSL_CTX_set_max_proto_version(ctx, TLS1_3_VERSION);

    X509 *cert = load_server_cert(paths->server_cert);
    if (!cert) { fprintf(stderr, "Failed to load server cert\n"); SSL_CTX_free(ctx); return NULL; }
    EVP_PKEY *key = load_server_key(paths->server_key);
    if (!key) { fprintf(stderr, "Failed to load server key\n"); X509_free(cert); SSL_CTX_free(ctx); return NULL; }
    X509 *ca = load_ca_cert(paths->ca_cert);
    if (!ca) { fprintf(stderr, "Failed to load CA cert\n"); EVP_PKEY_free(key); X509_free(cert); SSL_CTX_free(ctx); return NULL; }

    if (SSL_CTX_use_certificate(ctx, cert) <= 0) {
        fprintf(stderr, "Failed to set server cert\n"); goto fail;
    }
    if (SSL_CTX_use_PrivateKey(ctx, key) <= 0) {
        fprintf(stderr, "Failed to set server key\n"); goto fail;
    }

    X509_STORE *store = SSL_CTX_get_cert_store(ctx);
    X509_STORE_add_cert(store, ca);

    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, NULL);
    SSL_CTX_set_verify_depth(ctx, 1);

    EVP_PKEY_free(key);
    X509_free(cert);
    X509_free(ca);
    return ctx;

fail:
    EVP_PKEY_free(key);
    X509_free(cert);
    X509_free(ca);
    SSL_CTX_free(ctx);
    return NULL;
}

int tls_print_cert(const char *cert_dir, FILE *out) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/server.crt", cert_dir);
    X509 *x = load_server_cert(path);
    if (!x) return -1;
    PEM_write_X509(out, x);
#ifdef SECURE_BUILD
    tls_save_to_fingerprint_dir(cert_dir);
#endif
    X509_free(x);
    return 0;
}

int tls_print_ca(const char *cert_dir, FILE *out) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/ca.crt", cert_dir);
    X509 *x = load_ca_cert(path);
    if (!x) return -1;
    PEM_write_X509(out, x);
#ifdef SECURE_BUILD
    tls_save_to_fingerprint_dir(cert_dir);
#endif
    X509_free(x);
    return 0;
}

int tls_print_fingerprint(const char *cert_dir, FILE *out) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/server.crt", cert_dir);
    X509 *x = load_server_cert(path);
    if (!x) return -1;
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int md_len;
    X509_digest(x, EVP_sha256(), md, &md_len);
    for (unsigned int i = 0; i < md_len; i++)
        fprintf(out, "%02X%c", md[i], i < md_len - 1 ? ':' : '\n');
    X509_free(x);
    return 0;
}

int tls_print_pubkey_hex(const char *cert_dir, FILE *out) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/server.crt", cert_dir);
    X509 *x = load_server_cert(path);
    if (!x) return -1;
    EVP_PKEY *pkey = X509_get_pubkey(x);
    if (!pkey) { X509_free(x); return -1; }
    unsigned char pub[32];
    size_t len = sizeof(pub);
    if (EVP_PKEY_get_raw_public_key(pkey, pub, &len) <= 0) {
        EVP_PKEY_free(pkey); X509_free(x); return -1;
    }
    for (size_t i = 0; i < len; i++)
        fprintf(out, "%02x", pub[i]);
    fputc('\n', out);
    EVP_PKEY_free(pkey);
    X509_free(x);
    return 0;
}

static int write_der_as_c(FILE *out, const char *var_name,
                           const unsigned char *der, size_t len) {
    fprintf(out, "static const unsigned char %s[] = {\n", var_name);
    for (size_t i = 0; i < len; i++) {
        if (i % 12 == 0) fprintf(out, "  ");
        fprintf(out, "0x%02x,", der[i]);
        if (i % 12 == 11 || i == len - 1) fprintf(out, "\n");
    }
    fprintf(out, "};\n");
    fprintf(out, "static const unsigned int %s_len = sizeof(%s);\n\n", var_name, var_name);
    return 1;
}

int tls_embed_c_header(const char *cert_dir, const char *output_path) {
    // Load each PEM, convert to DER, write C arrays
    const char *files[] = {"ca.crt", "ca.key", "server.crt", "server.key"};
    const char *names[] = {"ca_crt", "ca_key", "server_crt", "server_key"};

    X509 *certs[4] = {NULL, NULL, NULL, NULL};
    EVP_PKEY *keys[4] = {NULL, NULL, NULL, NULL};
    int is_key[] = {0, 1, 0, 1};

    for (int i = 0; i < 4; i++) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", cert_dir, files[i]);
        if (is_key[i]) {
            keys[i] = pem_file_to_pkey(path);
            if (!keys[i]) { fprintf(stderr, "Failed to load %s\n", path); return -1; }
        } else {
            certs[i] = pem_file_to_x509(path);
            if (!certs[i]) { fprintf(stderr, "Failed to load %s\n", path); return -1; }
        }
    }

    FILE *out = fopen(output_path, "w");
    if (!out) { perror(output_path); return -1; }

    fprintf(out, "/* Generated by tls_embed_c_header — do not edit */\n");
    fprintf(out, "#ifndef CERTS_DATA_H\n#define CERTS_DATA_H\n\n");

    for (int i = 0; i < 4; i++) {
        unsigned char *der = NULL;
        int len;
        if (is_key[i]) {
            len = i2d_PrivateKey(keys[i], &der);
        } else {
            len = i2d_X509(certs[i], &der);
        }
        if (len <= 0 || !der) {
            fprintf(stderr, "Failed to DER-encode %s\n", files[i]);
            fclose(out);
            return -1;
        }
        write_der_as_c(out, names[i], der, len);
        OPENSSL_free(der);
    }

    fprintf(out, "#endif /* CERTS_DATA_H */\n");
    fclose(out);

    for (int i = 0; i < 4; i++) {
        if (certs[i]) X509_free(certs[i]);
        if (keys[i]) EVP_PKEY_free(keys[i]);
    }

    printf("Embedded header written to %s\n", output_path);
    return 0;
}

int tls_save_to_fingerprint_dir(const char *cert_dir) {
    char server_cert_path[1024];
    snprintf(server_cert_path, sizeof(server_cert_path), "%s/server.crt", cert_dir);
    X509 *x = load_server_cert(server_cert_path);
    if (!x) return -1;

    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int md_len;
    X509_digest(x, EVP_sha256(), md, &md_len);
    X509_free(x);

    char fp[65];
    for (unsigned int i = 0; i < md_len; i++)
        sprintf(fp + i * 2, "%02x", md[i]);
    fp[64] = '\0';

    char dest[1024];
    snprintf(dest, sizeof(dest), "%s/%s", cert_dir, fp);
    mkdir_p(dest);

#ifdef SECURE_BUILD
    (void)cert_dir;
    X509 *sv = der_to_x509(server_crt, server_crt_len);
    X509 *ca = der_to_x509(ca_crt, ca_crt_len);
    EVP_PKEY *sk = der_to_pkey(server_key, server_key_len);
    EVP_PKEY *ck = der_to_pkey(ca_key, ca_key_len);

    if (sv) { char p[1024]; snprintf(p,sizeof(p),"%s/server.crt",dest);
              FILE *f=fopen(p,"w"); if(f){PEM_write_X509(f,sv);fclose(f);chmod(p,0644);} X509_free(sv); }
    if (ca) { char p[1024]; snprintf(p,sizeof(p),"%s/ca.crt",dest);
              FILE *f=fopen(p,"w"); if(f){PEM_write_X509(f,ca);fclose(f);chmod(p,0644);} X509_free(ca); }
    if (sk) { char p[1024]; snprintf(p,sizeof(p),"%s/server.key",dest);
              FILE *f=fopen(p,"w"); if(f){PEM_write_PrivateKey(f,sk,NULL,NULL,0,NULL,NULL);fclose(f);chmod(p,0600);} EVP_PKEY_free(sk); }
    if (ck) { char p[1024]; snprintf(p,sizeof(p),"%s/ca.key",dest);
              FILE *f=fopen(p,"w"); if(f){PEM_write_PrivateKey(f,ck,NULL,NULL,0,NULL,NULL);fclose(f);chmod(p,0600);} EVP_PKEY_free(ck); }

    printf("Certificates saved to: %s\n", dest);
    return 0;
#else
    const char *files[] = {"ca.crt","ca.key","server.crt","server.key","client.crt","client.key"};
    for (int i = 0; i < 6; i++) {
        char src[1024], dst[1024];
        snprintf(src, sizeof(src), "%s/%s", cert_dir, files[i]);
        snprintf(dst, sizeof(dst), "%s/%s", dest, files[i]);
        FILE *fin = fopen(src, "r");
        if (!fin) continue;
        FILE *fout = fopen(dst, "w");
        if (!fout) { fclose(fin); continue; }
        char buf[8192];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), fin)) > 0)
            fwrite(buf, 1, n, fout);
        fclose(fin);
        fclose(fout);
        chmod(dst, strstr(files[i], ".key") ? 0600 : 0644);
    }
    printf("Certificates saved to: %s\n", dest);
    return 0;
#endif
}
