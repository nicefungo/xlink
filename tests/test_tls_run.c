/* test_tls_run.c — TLS non-blocking handshake via xlink_run()
 *
 * Tests the TLS handshake integration in xlink_run() event loop.
 * Uses the XLINK_NONBLOCK flag to enable non-blocking socket,
 * so the handshake is driven by the event loop step-by-step.
 *
 * Requires: OpenSSL (libssl-dev), certs at /tmp/xlink_test_*.pem
 * Build:   make tls
 * Run:     ./bin/tests/test_tls_run
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include "xlink.h"

static int nfail = 0;

#define CHECK(cond)                                               \
    do {                                                          \
        if (cond) printf("  PASS\n");                             \
        else { printf("  FAIL\n"); nfail++; }                     \
    } while (0)

/* ─── Callback context ───────────────────────────────── */
typedef struct {
    int  count;
    int  hs_done_cnt;
    int  data_events;
    int  hs_failed;
    char last_buf[128];
    size_t last_len;
} tls_run_ctx_t;

static int tls_run_cb(xlink_channel_t **chans, int n, int ch_idx, void *arg) {
    (void)n;
    tls_run_ctx_t *ctx = arg;
    ctx->count++;

    int hs = xlink_tls_handshake_state(chans[ch_idx]);
    if (hs == -1) {
        ctx->hs_failed++;
        return 1;
    }

    /* Handshake done and TLS enabled — try to recv data */
    if (hs == 0 && xlink_tls_enabled(chans[ch_idx])) {
        size_t len = sizeof(ctx->last_buf);
        int rc = xlink_recv(chans[ch_idx], ctx->last_buf, &len);
        if (rc == 0 && len > 0) {
            ctx->data_events++;
            ctx->last_len = len;
            return 1;  /* got data, stop */
        }
        ctx->hs_done_cnt++;
        return 0;  /* no data yet, keep waiting */
    }

    /* Still handshaking — shouldn't be called back unless something wrong */
    return 1;
}

/* ─── Test 1: non-blocking TLS client handshake via xlink_run ─ */
static void test_nb_client(void) {
    printf("\n--- Test 1: non-blocking TLS client handshake ---\n");

    xlink_tls_config_t cfg = {
        .cert_file   = "/tmp/xlink_test_cert.pem",
        .key_file    = "/tmp/xlink_test_key.pem",
        .verify_peer = 0,
    };

    /* Fork: blocking TLS echo server */
    pid_t pid = fork();
    if (pid == 0) {
        xlink_opt_t opt = { .flags = XLINK_SERVER | XLINK_TLS };
        xlink_channel_t *ch = xlink_open(XLINK_TCP, ":19962", &opt);
        if (!ch) _exit(1);
        if (xlink_tls_configure(ch, &cfg) != 0) { xlink_close(ch); _exit(1); }

        /* Accept + echo one message */
        void *aio = xlink_aio_create(2);
        xlink_channel_t *chans[] = {ch};
        int idx = xlink_wait_aio(chans, 1, 8000, aio);
        xlink_aio_destroy(aio);
        if (idx >= 0) {
            size_t len = 256;
            char buf[256] = {0};
            int rc = xlink_recv(ch, buf, &len);
            if (rc == 0 && len > 0)
                xlink_send(ch, buf, len);
        }
        xlink_close(ch);
        _exit(0);
    }

    sleep(1);

    /* Client: non-blocking + TLS.
     * The handshake will be driven by xlink_run(). */
    xlink_opt_t opt = { .flags = XLINK_TLS | XLINK_NONBLOCK };
    xlink_channel_t *ch = xlink_open(XLINK_TCP, "127.0.0.1:19962", &opt);
    if (!ch) {
        printf("  SKIP: client open failed\n");
        waitpid(pid, NULL, 0);
        return;
    }
    if (xlink_tls_configure(ch, &cfg) != 0) {
        printf("  SKIP: configure failed: %s\n", xlink_errstr(ch));
        xlink_close(ch);
        waitpid(pid, NULL, 0);
        return;
    }

    /* After handshake completes (HS_DONE callback), send data to server,
     * then recv echo.  But the callback prototype doesn't let us easily
     * conditionally send.  We test: the handshake completes via xlink_run. */
    tls_run_ctx_t ctx = {0};
    xlink_channel_t *chans[] = {ch};
    int rc = xlink_run(chans, 1, 3000, NULL, tls_run_cb, &ctx);
    waitpid(pid, NULL, 0);

    CHECK(rc == 0);
    printf("    xlink_run returned (rc=%d)\n", rc);
    CHECK(ctx.count > 0);
    printf("    callback calls: %d\n", ctx.count);
    CHECK(ctx.hs_failed == 0);
    printf("    handshake failures: %d\n", ctx.hs_failed);

    /* Verify TLS is established (hs == DONE) */
    int hs = xlink_tls_handshake_state(ch);
    CHECK(hs == 0);
    printf("    handshake state: %d (0=DONE)\n", hs);

    xlink_close(ch);
}

/* ─── Test 2: non-blocking TLS server handshake via xlink_run ─ */
static void test_nb_server(void) {
    printf("\n--- Test 2: non-blocking TLS server handshake ---\n");

    xlink_tls_config_t cfg = {
        .cert_file   = "/tmp/xlink_test_cert.pem",
        .key_file    = "/tmp/xlink_test_key.pem",
        .verify_peer = 0,
    };

    /* Fork: blocking TLS client that connects and sends */
    pid_t pid = fork();
    if (pid == 0) {
        sleep(1);
        xlink_opt_t opt = { .flags = XLINK_TLS };
        xlink_channel_t *ch = xlink_open(XLINK_TCP, "127.0.0.1:19963", &opt);
        if (!ch) _exit(1);
        if (xlink_tls_configure(ch, &cfg) != 0) { xlink_close(ch); _exit(1); }

        xlink_send(ch, "HELLO", 5);
        size_t len = 256;
        char buf[256];
        xlink_recv(ch, buf, &len);
        xlink_close(ch);
        _exit(0);
    }

    /* Server: non-blocking + TLS */
    xlink_opt_t opt = { .flags = XLINK_SERVER | XLINK_TLS | XLINK_NONBLOCK };
    xlink_channel_t *ch = xlink_open(XLINK_TCP, ":19963", &opt);
    if (!ch) {
        printf("  SKIP: server open failed\n");
        waitpid(pid, NULL, 0);
        return;
    }
    if (xlink_tls_configure(ch, &cfg) != 0) {
        printf("  SKIP: configure failed: %s\n", xlink_errstr(ch));
        xlink_close(ch);
        waitpid(pid, NULL, 0);
        return;
    }

    tls_run_ctx_t ctx = {0};
    xlink_channel_t *chans[] = {ch};
    int rc = xlink_run(chans, 1, 10000, NULL, tls_run_cb, &ctx);
    waitpid(pid, NULL, 0);

    CHECK(rc == 0);
    printf("    xlink_run returned (rc=%d)\n", rc);
    CHECK(ctx.count > 0);
    printf("    callback calls: %d\n", ctx.count);
    CHECK(ctx.hs_failed == 0);
    printf("    handshake failures: %d\n", ctx.hs_failed);

    int hs = xlink_tls_handshake_state(ch);
    CHECK(hs == 0);
    printf("    handshake state: %d (0=DONE)\n", hs);
    CHECK(ctx.data_events > 0);
    printf("    data events: %d\n", ctx.data_events);
    if (ctx.last_len > 0) {
        int ok = (ctx.last_len == 5 && memcmp(ctx.last_buf, "HELLO", 5) == 0);
        CHECK(ok);
        printf("    received: '%.*s'\n", (int)ctx.last_len, ctx.last_buf);
    }

    xlink_close(ch);
}

/* ─── Test 3: handshake state transitions ─────────────── */
static void test_hs_state(void) {
    printf("\n--- Test 3: handshake state before/after xlink_run ---\n");

    xlink_tls_config_t cfg = {
        .cert_file   = "/tmp/xlink_test_cert.pem",
        .key_file    = "/tmp/xlink_test_key.pem",
        .verify_peer = 0,
    };

    /* Open a non-blocking TLS channel (no server — will fail/timeout) */
    xlink_opt_t opt = { .flags = XLINK_TLS | XLINK_NONBLOCK };
    xlink_channel_t *ch = xlink_open(XLINK_TCP, "127.0.0.1:19969", &opt);
    if (!ch) {
        printf("  SKIP: open failed\n");
        return;
    }
    if (xlink_tls_configure(ch, &cfg) != 0) {
        xlink_close(ch);
        return;
    }

    /* Before any I/O, handshake state should be IDLE */
    int hs = xlink_tls_handshake_state(ch);
    CHECK(hs == 0);
    printf("    state before first I/O: %d (expect IDLE)\n", hs);

    /* Call xlink_run with short timeout — no server, so handshake
     * attempt will fail or time out.  The point is: handshake_state
     * should transition from IDLE to WANT_READ/WANT_WRITE/FAILED. */
    tls_run_ctx_t ctx = {0};
    xlink_channel_t *chans[] = {ch};
    xlink_run(chans, 1, 500, NULL, tls_run_cb, &ctx);

    hs = xlink_tls_handshake_state(ch);
    printf("    state after xlink_run: %d (0=DONE,1=WANT_RD,2=WANT_WR,-1=FAIL)\n", hs);
    /* State should have changed from IDLE — either still pending or failed */
    CHECK(hs != 0);  /* No server means it should either fail or still be in progress */

    xlink_close(ch);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("=== TLS xlink_run() integration tests ===\n");

    FILE *f = fopen("/tmp/xlink_test_cert.pem", "r");
    if (!f) {
        printf("SKIP: TLS certs not found\n");
        return 0;
    }
    fclose(f);

    test_nb_client();
    test_nb_server();
    test_hs_state();

    printf("\n=== %d failures ===\n", nfail);
    return nfail ? 1 : 0;
}
