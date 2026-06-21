#include "tls.h"
#include "server.h"
#include "handler.h"
#include "forward.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>

static server_t g_server;
static char **g_argv;
static pthread_mutex_t g_shutdown_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_shutdown_cond = PTHREAD_COND_INITIALIZER;
static int g_shutdown_flag = 0;

static void handle_signal(int sig) {
    (void)sig;
    pthread_mutex_lock(&g_shutdown_mutex);
    g_shutdown_flag = 1;
    pthread_cond_signal(&g_shutdown_cond);
    pthread_mutex_unlock(&g_shutdown_mutex);
    server_stop(&g_server);
}

static void print_usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [options]\n"
        "Options:\n"
        "  --daemon               Run as daemon (fork to background)\n"
        "  --init-certs           Generate TLS certificates and exit\n"
        "  --cert-dir <dir>       Certificate directory\n"
        "                         (default: ~/.termux-adb-bridge/certs)\n"
        "  --address <addr>       Bind address (default: 127.0.0.1)\n"
        "  --port <port>          Bind port (default: 10099)\n"
        "  --cert                 Print server certificate (PEM) to stdout\n"
        "  --ca                   Print CA certificate (PEM) to stdout\n"
        "  --fingerprint          Print SHA256 fingerprint of server cert\n"
        "  --modulus              Print ED25519 public key hex\n"
        "  --save-certs            Save PEM certs to <cert-dir>/<fingerprint>/\n"
        "  --embed-c-header <file>\n"
        "                         Generate C header with embedded DER certs\n"
        "  --help                 Show this help\n",
        prog);
}

int main(int argc, char *argv[]) {
    g_argv = argv;
    const char *cert_dir = NULL;
    char default_cert_dir[1024];
    int daemon_mode = 0;
    int init_certs = 0;
    const char *address = "127.0.0.1";
    int port = 10099;

    int cert_mode = 0;
    int ca_mode = 0;
    int fingerprint = 0;
    int modulus = 0;
    int save_certs = 0;
    int embed_hdr = 0;
#ifndef SECURE_BUILD
    const char *embed_hdr_path = NULL;
#endif

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "--daemon") == 0) {
            daemon_mode = 1;
        } else if (strcmp(argv[i], "--init-certs") == 0) {
            init_certs = 1;
        } else if (strcmp(argv[i], "--cert-dir") == 0 && i + 1 < argc) {
            cert_dir = argv[++i];
        } else if (strcmp(argv[i], "--address") == 0 && i + 1 < argc) {
            address = argv[++i];
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--cert") == 0) {
            cert_mode = 1;
        } else if (strcmp(argv[i], "--ca") == 0) {
            ca_mode = 1;
        } else if (strcmp(argv[i], "--fingerprint") == 0) {
            fingerprint = 1;
        } else if (strcmp(argv[i], "--modulus") == 0) {
            modulus = 1;
        } else if (strcmp(argv[i], "--save-certs") == 0) {
            save_certs = 1;
        } else if (strcmp(argv[i], "--embed-c-header") == 0 && i + 1 < argc) {
            embed_hdr = 1;
#ifndef SECURE_BUILD
            embed_hdr_path = argv[++i];
#else
            ++i;
#endif
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    if (!cert_dir) {
        const char *home = getenv("HOME");
        if (!home) home = "/data/data/com.termux/files/home";
        snprintf(default_cert_dir, sizeof(default_cert_dir),
                 "%s/.termux-adb-bridge/certs", home);
        cert_dir = default_cert_dir;
    }

    int mode_count = cert_mode + ca_mode + fingerprint + modulus + embed_hdr + save_certs + init_certs;
    if (mode_count > 1) {
        fprintf(stderr, "Error: conflicting flags\n");
        return 1;
    }

    if (init_certs) {
#ifdef SECURE_BUILD
        fprintf(stderr, "Error: --init-certs not available in secure build (certs are baked in)\n");
        return 1;
#else
        tls_init();
        int r = tls_generate_certs(cert_dir);
        if (r == 0) {
            printf("Certificates generated successfully in: %s\n", cert_dir);
            printf("  ca.crt      - CA certificate (trust anchor)\n");
            printf("  ca.key      - CA private key\n");
            printf("  server.crt  - Server certificate\n");
            printf("  server.key  - Server private key\n");
            printf("  client.crt  - Client certificate (for desktop)\n");
            printf("  client.key  - Client private key (for desktop)\n");
        }
        return r;
#endif
    }

    if (cert_mode) {
        tls_init();
        return tls_print_cert(cert_dir, stdout) == 0 ? 0 : 1;
    }
    if (ca_mode) {
        tls_init();
        return tls_print_ca(cert_dir, stdout) == 0 ? 0 : 1;
    }
    if (fingerprint) {
        tls_init();
        return tls_print_fingerprint(cert_dir, stdout) == 0 ? 0 : 1;
    }
    if (modulus) {
        tls_init();
        return tls_print_pubkey_hex(cert_dir, stdout) == 0 ? 0 : 1;
    }
    if (embed_hdr) {
#ifdef SECURE_BUILD
        fprintf(stderr, "Error: --embed-c-header not available in secure build\n");
        return 1;
#else
        tls_init();
        return tls_embed_c_header(cert_dir, embed_hdr_path) == 0 ? 0 : 1;
#endif
    }

    if (save_certs) {
        tls_init();
        return tls_save_to_fingerprint_dir(cert_dir) == 0 ? 0 : 1;
    }

    signal(SIGHUP, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    if (daemon_mode) {
        pid_t pid = fork();
        if (pid < 0) { perror("fork"); return 1; }
        if (pid > 0) {
            printf("Daemon started with PID %d\n", pid);
            _exit(0);
        }
        setsid();
        freopen("/dev/null", "r", stdin);
        freopen("/dev/null", "w", stdout);
        freopen("/dev/null", "w", stderr);
    }

    if (!tls_init()) {
        fprintf(stderr, "TLS initialization failed\n");
        return 1;
    }

    cert_paths_t paths;
    tls_build_paths(&paths, cert_dir);

    X509 *ca_cert = NULL, *server_cert = NULL;
    SSL_CTX *ctx = tls_create_server_ctx(&paths, &ca_cert, &server_cert);
    if (!ctx) {
        fprintf(stderr, "Failed to create SSL context\n");
        return 1;
    }

    g_server.ssl_ctx = ctx;
#ifdef SECURE_BUILD
    tls_save_to_fingerprint_dir(cert_dir);
#endif
    g_server.port = port;
    strncpy(g_server.address, address, sizeof(g_server.address) - 1);
    g_server.running = 0;

    if (!daemon_mode) {
        printf("Starting server on %s:%d\n", address, port);
        printf("Certificates: %s\n", cert_dir);
    }

    handler_set_cert_dir(cert_dir);
    handler_set_certs(ca_cert, server_cert);
    handler_set_argv(g_argv);
    handler_set_listen_fd(-1);
    server_start(&g_server);

    forward_shutdown_all();
    SSL_CTX_free(ctx);
    X509_free(ca_cert);
    X509_free(server_cert);
    return 0;
}
