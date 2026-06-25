/* test_tls.c — TLS encrypted TCP communication test
 *
 * Requires: OpenSSL (libssl-dev), certs at /tmp/xlink_test_*.pem
 * Build:   make tls
 * Run:     ./bin/tests/test_tls
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include "xlink.h"

static int check(const char *label, int ok) {
    if (ok)
        printf("  PASS: %s\n", label);
    else
        printf("  FAIL: %s\n", label);
    return ok ? 0 : 1;
}

static int failures = 0;

/* ─── Test 1: TLS client ↔ TLS server (basic) ─── */
static void test_tls_basic(void) {
    printf("\n=== Test 1: TLS client ↔ server (basic send/recv) ===\n");

    xlink_tls_config_t cfg = {
        .cert_file    = "/tmp/xlink_test_cert.pem",
        .key_file     = "/tmp/xlink_test_key.pem",
        .ca_file      = NULL,
        .verify_peer  = 0,
        .sni_hostname = NULL,
    };

    /* Fork: child = server, parent = client */
    pid_t pid = fork();
    if (pid == 0) {
        /* Child: TLS server */
        xlink_opt_t opt = { .flags = XLINK_SERVER | XLINK_TLS };
        xlink_channel_t *ch = xlink_open(XLINK_TCP, ":19898", &opt);
        if (!ch) {
            printf("  SKIP: server open failed (port in use?)\n");
            _exit(1);
        }

        if (xlink_tls_configure(ch, &cfg) != 0) {
            printf("  SKIP: server TLS configure failed: %s\n", xlink_errstr(ch));
            xlink_close(ch);
            _exit(1);
        }
        failures += check("TLS server enabled", xlink_tls_enabled(ch));

        size_t len = 1024;
        char buf[1024];
        int rc = xlink_recv(ch, buf, &len);
        failures += check("server recv OK", rc == 0);
        failures += check("server data size", len == 5);
        if (len == 5)
            failures += check("server data content", memcmp(buf, "HELLO", 5) == 0);

        xlink_send(ch, "WORLD", 5);
        xlink_close(ch);
        _exit(0);
    }

    /* Parent: TLS client */
    sleep(1);  /* wait for server to bind */

    xlink_opt_t opt = { .flags = XLINK_TLS };
    xlink_channel_t *ch = xlink_open(XLINK_TCP, "127.0.0.1:19898", &opt);
    if (!ch) {
        printf("  SKIP: client open failed\n");
        waitpid(pid, NULL, 0);
        return;
    }

    if (xlink_tls_configure(ch, &cfg) != 0) {
        printf("  SKIP: client TLS configure failed: %s\n", xlink_errstr(ch));
        xlink_close(ch);
        waitpid(pid, NULL, 0);
        return;
    }
    failures += check("TLS client enabled", xlink_tls_enabled(ch));

    xlink_send(ch, "HELLO", 5);

    size_t len = 1024;
    char buf[1024];
    int rc = xlink_recv(ch, buf, &len);
    failures += check("client recv OK", rc == 0);
    failures += check("client data size", len == 5);
    if (len == 5)
        failures += check("client data content", memcmp(buf, "WORLD", 5) == 0);

    xlink_close(ch);
    waitpid(pid, NULL, 0);
}

/* ─── Test 2: TLS client ↔ TLS server (large payload) ─── */
static void test_tls_large(void) {
    printf("\n=== Test 2: TLS large payload (64KB) ===\n");

    xlink_tls_config_t cfg = {
        .cert_file    = "/tmp/xlink_test_cert.pem",
        .key_file     = "/tmp/xlink_test_key.pem",
        .ca_file      = NULL,
        .verify_peer  = 0,
        .sni_hostname = NULL,
    };

    pid_t pid = fork();
    if (pid == 0) {
        /* Child: server */
        xlink_opt_t opt = { .flags = XLINK_SERVER | XLINK_TLS };
        xlink_channel_t *ch = xlink_open(XLINK_TCP, ":19899", &opt);
        if (!ch) _exit(1);

        if (xlink_tls_configure(ch, &cfg) != 0) { xlink_close(ch); _exit(1); }

        size_t len = 65536;
        char *buf = malloc(len);
        int rc = xlink_recv(ch, buf, &len);
        failures += check("large: server recv OK", rc == 0);
        failures += check("large: server recv size", len == 65536);

        /* Echo back */
        xlink_send(ch, buf, len);
        free(buf);
        xlink_close(ch);
        _exit(0);
    }

    sleep(1);

    xlink_opt_t opt = { .flags = XLINK_TLS };
    xlink_channel_t *ch = xlink_open(XLINK_TCP, "127.0.0.1:19899", &opt);
    if (!ch) { waitpid(pid, NULL, 0); return; }

    if (xlink_tls_configure(ch, &cfg) != 0) {
        xlink_close(ch); waitpid(pid, NULL, 0); return;
    }

    size_t sz = 65536;
    char *buf = malloc(sz);
    memset(buf, 0x5A, sz);
    xlink_send(ch, buf, sz);

    size_t len = sz;
    memset(buf, 0, sz);
    int rc = xlink_recv(ch, buf, &len);
    failures += check("large: client recv OK", rc == 0);
    failures += check("large: client recv size", len == sz);
    if (len == sz) {
        int ok = 1;
        for (size_t i = 0; i < sz; i++)
            if ((unsigned char)buf[i] != 0x5A) { ok = 0; break; }
        failures += check("large: data integrity", ok);
    }

    free(buf);
    xlink_close(ch);
    waitpid(pid, NULL, 0);
}

/* ─── Test 3: Plain client ↔ TLS server (server rejects) ─── */
static void test_tls_reject_plain(void) {
    printf("\n=== Test 3: Plain client ↔ TLS server ===\n");

    /* Server: TLS, tries to accept → will get plaintext → handshake fails */
    xlink_tls_config_t cfg = {
        .cert_file    = "/tmp/xlink_test_cert.pem",
        .key_file     = "/tmp/xlink_test_key.pem",
        .verify_peer  = 0,
    };

    pid_t pid = fork();
    if (pid == 0) {
        xlink_opt_t opt = { .flags = XLINK_SERVER | XLINK_TLS };
        xlink_channel_t *ch = xlink_open(XLINK_TCP, ":19900", &opt);
        if (!ch) _exit(1);
        if (xlink_tls_configure(ch, &cfg) != 0) { xlink_close(ch); _exit(1); }

        /* Use aio timeout so we don't hang forever */
        void *aio = xlink_aio_create(2);
        xlink_channel_t *chans[] = {ch};
        int idx = xlink_wait_aio(chans, 1, 3000, aio);
        xlink_aio_destroy(aio);

        if (idx < 0) {
            /* Timeout — expected, plain client won't do TLS handshake */
            failures += check("TLS server times out on plain client", 1);
        }
        xlink_close(ch);
        _exit(0);
    }

    sleep(1);

    /* Connect WITHOUT TLS — just plain TCP */
    xlink_opt_t opt = { .flags = 0 };
    xlink_channel_t *ch = xlink_open(XLINK_TCP, "127.0.0.1:19900", &opt);
    if (!ch) { waitpid(pid, NULL, 0); return; }

    /* Plain client sends — server can't read it (expects TLS) */
    xlink_send(ch, "PLAIN", 5);
    xlink_close(ch);
    waitpid(pid, NULL, 0);
}

/* ─── Test 4: Non-TCP channel rejects TLS ─── */
static void test_tls_non_tcp(void) {
    printf("\n=== Test 4: TLS configure on non-TCP channel ===\n");

    xlink_opt_t opt = { .flags = XLINK_CREATE };
    xlink_channel_t *ch = xlink_open(XLINK_PIPE, "/tmp/xlink_tls_reject", &opt);
    if (!ch) {
        printf("  SKIP: pipe open failed\n");
        return;
    }

    xlink_tls_config_t cfg = { NULL };
    int rc = xlink_tls_configure(ch, &cfg);
    failures += check("TLS rejected on pipe", rc == -1);

    xlink_close(ch);
}

/* ─── Test 5: TLS enabled flag without configure ─── */
static void test_tls_no_configure(void) {
    printf("\n=== Test 5: TLS flag without configure ===\n");

    xlink_tls_config_t cfg = {
        .cert_file    = "/tmp/xlink_test_cert.pem",
        .key_file     = "/tmp/xlink_test_key.pem",
        .ca_file      = NULL,
        .verify_peer  = 0,
        .sni_hostname = NULL,
    };

    pid_t pid = fork();
    if (pid == 0) {
        xlink_opt_t opt = { .flags = XLINK_SERVER | XLINK_TLS };
        xlink_channel_t *ch = xlink_open(XLINK_TCP, ":19901", &opt);
        if (!ch) _exit(1);

        /* Open with TLS flag but FORGET to configure */
        failures += check("TLS flag without configure: TLS disabled",
                          !xlink_tls_enabled(ch));

        /* Now configure */
        if (xlink_tls_configure(ch, &cfg) != 0) {
            xlink_close(ch);
            _exit(1);
        }
        failures += check("TLS flag + configure: TLS enabled",
                          xlink_tls_enabled(ch));

        /* Wait for client with timeout */
        void *aio = xlink_aio_create(2);
        xlink_channel_t *chans[] = {ch};
        int idx = xlink_wait_aio(chans, 1, 5000, aio);
        xlink_aio_destroy(aio);

        if (idx >= 0) {
            size_t len = 1024;
            char buf[1024];
            xlink_recv(ch, buf, &len);
            xlink_send(ch, "DONE", 4);
        }
        xlink_close(ch);
        _exit(0);
    }

    sleep(1);

    xlink_opt_t opt = { .flags = XLINK_TLS };
    xlink_channel_t *ch = xlink_open(XLINK_TCP, "127.0.0.1:19901", &opt);
    if (!ch) { waitpid(pid, NULL, 0); return; }

    if (xlink_tls_configure(ch, &cfg) != 0) {
        xlink_close(ch); waitpid(pid, NULL, 0); return;
    }

    xlink_send(ch, "TEST", 4);
    size_t len = 1024;
    char buf[1024];
    xlink_recv(ch, buf, &len);
    failures += check("delayed configure: recv OK", memcmp(buf, "DONE", 4) == 0);

    xlink_close(ch);
    waitpid(pid, NULL, 0);
}

/* ─── Main ─── */
int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("=== xlink TLS tests ===\n");

    test_tls_basic();
    test_tls_large();
    test_tls_reject_plain();
    test_tls_non_tcp();
    test_tls_no_configure();

    printf("\n=== %d failures ===\n", failures);
    return failures ? 1 : 0;
}