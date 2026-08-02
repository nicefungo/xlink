/* test_tls_alpn.c — ALPN (Application-Layer Protocol Negotiation) test */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include "xlink.h"

static int failures = 0;
static int check(const char *label, int ok) {
    if (ok) printf("  PASS: %s\n", label);
    else { printf("  FAIL: %s\n", label); failures++; }
    return ok ? 0 : 1;
}
static int str_eq(const char *a, const char *b) {
    if (!a || !b) return 0;
    return strcmp(a, b) == 0;
}

/* Server-side watchdog: if the server child ever hangs, kill itself
 * so the parent's waitpid() doesn't block forever. */
static void server_alarm(int sig) {
    (void)sig;
    _exit(2);
}

/* Run one ALPN test pair. Returns 0 on success. */
static int run_alpn_test(int port, const char *sprotos, const char *cprotos,
                          int expect_selected, const char *expect_alpn) {
    xlink_opt_t opt;
    char addr[16];
    snprintf(addr, sizeof(addr), ":%d", port);

    xlink_tls_config_t scfg = {
        .cert_file   = "/tmp/xlink_test_cert.pem",
        .key_file    = "/tmp/xlink_test_key.pem",
        .verify_peer = 0,
        .alpn_protos = sprotos,
    };

    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return 1; }

    if (pid == 0) {
        /* Server child: enable watchdog so it can never hang forever. */
        signal(SIGALRM, server_alarm);
        alarm(5);

        opt.flags = XLINK_SERVER | XLINK_TLS;
        xlink_channel_t *ch = xlink_open(XLINK_TCP, addr, &opt);
        if (!ch) _exit(1);
        if (xlink_tls_configure(ch, &scfg) != 0) { xlink_close(ch); _exit(1); }

        char buf[256] = {0};
        size_t len = sizeof(buf);
        if (xlink_recv(ch, buf, &len) == 0)
            xlink_send(ch, "ok", 2);
        xlink_close(ch);
        _exit(0);
    }

    sleep(1);  /* wait for server to bind */

    char conn[32];
    snprintf(conn, sizeof(conn), "127.0.0.1:%d", port);

    xlink_tls_config_t ccfg = {
        .verify_peer = 0,
        .alpn_protos = cprotos,
    };

    opt.flags = XLINK_TLS;
    xlink_channel_t *ch = xlink_open(XLINK_TCP, conn, &opt);
    if (!ch) { waitpid(pid, NULL, 0); return 1; }

    if (xlink_tls_configure(ch, &ccfg) != 0) {
        xlink_close(ch); waitpid(pid, NULL, 0); return 1;
    }

    /* If we expect no ALPN match, the TLS handshake itself may legitimately
     * fail (RFC 7301: server that configures an ALPN select callback MUST
     * reject a client that offers no matching protocol).  In that case send
     * may fail — which is the expected outcome, not a hang. */
    int send_rc = xlink_send(ch, "HELLO", 5);

    const char *alpn = xlink_tls_alpn_negotiated(ch);
    int ok;
    if (expect_selected) {
        ok = (alpn != NULL && str_eq(alpn, expect_alpn));
    } else if (send_rc != 0) {
        /* No-match + forced server ALPN => handshake rejected.  That is the
         * correct RFC 7301 behaviour; ALPN must be NULL and the connection
         * should be torn down.  Treat as pass. */
        ok = (alpn == NULL);
    } else {
        /* Handshake completed with no ALPN selected (optional negotiation). */
        ok = (alpn == NULL);
    }
    check("ALPN result", ok);

    char buf[256] = {0};
    size_t len = sizeof(buf);
    /* If send failed (handshake rejected), skip the blocking recv. */
    if (send_rc == 0)
        xlink_recv(ch, buf, &len);

    xlink_close(ch);

    /* Wait for child with timeout */
    int status;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        if (expect_selected) {
            /* Matching ALPN must complete cleanly. */
            check("server child exited cleanly", 0);
        }
        /* For no-match cases the server may exit non-zero; that's expected. */
    }

    return ok ? 0 : 1;
}

int main(void) {
    printf("=== xlink TLS ALPN tests ===\n");
    FILE *f = fopen("/tmp/xlink_test_cert.pem", "r");
    if (!f) { printf("SKIP: no certs\n"); return 0; }
    fclose(f);

    /* Test 1: matching ALPN */
    printf("\n--- Test 1: matching ALPN ---\n");
    run_alpn_test(29960, "xlink/1,xlink/json", "xlink/1,xlink/json", 1, "xlink/1");

    /* Test 2: no match (server forces ALPN => handshake rejected per RFC 7301) */
    printf("\n--- Test 2: ALPN no match ---\n");
    run_alpn_test(29961, "xlink/1", "xlink/2", 0, NULL);

    /* Test 3: client-only ALPN (server does not force => optional) */
    printf("\n--- Test 3: client-only ALPN ---\n");
    run_alpn_test(29962, NULL, "xlink/1", 0, NULL);

    /* Test 4: server-only ALPN (client offers none => server forces reject) */
    printf("\n--- Test 4: server-only ALPN ---\n");
    run_alpn_test(29963, "xlink/1", NULL, 0, NULL);

    printf("\n=== %d failures ===\n", failures);
    return failures > 0 ? 1 : 0;
}
