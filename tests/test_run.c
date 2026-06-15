/* test_run.c — xlink_run() event loop tests */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include "xlink.h"

#define PASS(fmt, ...) printf("  PASS: " fmt "\n", ##__VA_ARGS__)
#define FAIL(fmt, ...)                     \
    do {                                   \
        printf("  FAIL: " fmt "\n", ##__VA_ARGS__); \
        return 1;                          \
    } while (0)

static int npass = 0, nfail = 0;

#define CHECK(cond, msg)                                                \
    do {                                                                \
        if (cond) { PASS(msg); npass++; }                               \
        else { FAIL(msg); nfail++; }                                    \
    } while (0)

/* ─── Callback helpers ─── */

typedef struct {
    int    count;
    int    last_idx;
    int    stop_at;
    int    drain;  /* if 1, also recv data from ch_idx */
} cb_ctx_t;

static int count_cb(xlink_channel_t **chans, int n, int ch_idx, void *arg) {
    (void)n;
    cb_ctx_t *ctx = (cb_ctx_t *)arg;
    ctx->count++;
    ctx->last_idx = ch_idx;
    if (ctx->drain) {
        char buf[64];
        size_t len = sizeof(buf);
        xlink_recv(chans[ch_idx], buf, &len);
    }
    if (ctx->stop_at > 0 && ctx->count >= ctx->stop_at) return 1;
    return 0;
}

static int timeout_test_cb(xlink_channel_t **chans, int n, int ch_idx, void *arg) {
    (void)chans; (void)n; (void)ch_idx;
    int *called = (int *)arg;
    *called = 1;
    return 1;
}

/* ─── Thread helper: delayed send ─── */

typedef struct {
    const char *shm_name;
    int         delay_ms;
} thread_args_t;

/* Open sender WITHOUT CREATE — SHM+FIFO already created by receiver */
static void *delayed_sender(void *arg) {
    thread_args_t *ta = (thread_args_t *)arg;
    usleep((unsigned int)(ta->delay_ms * 1000));

    xlink_channel_t *tx = xlink_open(XLINK_SHM, ta->shm_name, NULL);
    if (!tx) return (void*)(intptr_t)-1;

    xlink_send(tx, "ping", 4);
    xlink_close(tx);
    return (void*)(intptr_t)0;
}

static void *delayed_pipe_writer(void *arg) {
    thread_args_t *ta = (thread_args_t *)arg;
    usleep((unsigned int)(ta->delay_ms * 1000));

    xlink_channel_t *tx = xlink_open(XLINK_PIPE, ta->shm_name, NULL);
    if (!tx) return (void*)(intptr_t)-1;

    xlink_send(tx, "hello", 5);
    xlink_close(tx);
    return (void*)(intptr_t)0;
}

/* Send 5 messages with pacing — SHM slot size is 1,
 * so each send must be consumed before the next (shm_writen is blocking). */
static void *delayed_multi_sender(void *arg) {
    thread_args_t *ta = (thread_args_t *)arg;
    usleep((unsigned int)(ta->delay_ms * 1000));

    xlink_channel_t *tx = xlink_open(XLINK_SHM, ta->shm_name, NULL);
    if (!tx) return (void*)(intptr_t)-1;

    for (int i = 0; i < 5; i++) {
        xlink_send(tx, "m", 1);
        usleep(50000);  /* 50ms between sends */
    }
    xlink_close(tx);
    return (void*)(intptr_t)0;
}

/* Helper: remove a file safely (silence system() unused-result warning) */
static void rm_f(const char *path) {
    unlink(path);
}

int main(void) {
    printf("=== xlink_run() event loop tests ===\n\n");

    /* ─── Single SHM channel, one event ─── */
    printf("--- Single SHM, one event ---\n");

    xlink_opt_t opt = XLINK_OPT_DEFAULT;
    opt.flags = XLINK_CREATE;

    xlink_channel_t *rx = xlink_open(XLINK_SHM, "run_shm_1", &opt);
    CHECK(rx != NULL, "open SHM receiver");

    void *aio = xlink_aio_create(0);
    CHECK(aio != NULL, "create aio engine");

    cb_ctx_t ctx = {0, -1, 1, 0};  /* stop after 1 event */

    /* Spawn sender thread */
    pthread_t thr;
    thread_args_t ta = {"run_shm_1", 50};
    pthread_create(&thr, NULL, delayed_sender, &ta);

    int rc = xlink_run(&rx, 1, 2000, aio, count_cb, &ctx);
    pthread_join(thr, NULL);

    CHECK(rc == 0, "xlink_run returns 0 (normal exit)");
    CHECK(ctx.count == 1, "callback called once");
    CHECK(ctx.last_idx == 0, "callback got index 0");

    xlink_aio_destroy(aio);
    xlink_close(rx);

    /* ─── Pipe channel, one event ─── */
    printf("\n--- Pipe channel, one event ---\n");

    rm_f("/tmp/run_pipe_1");
    opt.flags = XLINK_CREATE;
    xlink_channel_t *p_rx = xlink_open(XLINK_PIPE, "/tmp/run_pipe_1", &opt);
    CHECK(p_rx != NULL, "open pipe receiver");

    aio = xlink_aio_create(0);
    CHECK(aio != NULL, "create aio engine");

    cb_ctx_t pctx = {0, -1, 1, 0};
    thread_args_t pta = {"/tmp/run_pipe_1", 50};
    pthread_create(&thr, NULL, delayed_pipe_writer, &pta);

    rc = xlink_run(&p_rx, 1, 2000, aio, count_cb, &pctx);
    pthread_join(thr, NULL);

    CHECK(rc == 0, "xlink_run on pipe returns 0");
    CHECK(pctx.count == 1, "pipe callback called once");
    CHECK(pctx.last_idx == 0, "pipe callback got index 0");

    xlink_aio_destroy(aio);
    xlink_close(p_rx);
    rm_f("/tmp/run_pipe_1");

    /* ─── Multiple events (5) ─── */
    printf("\n--- Multiple events (5) ---\n");

    xlink_channel_t *m_rx = xlink_open(XLINK_SHM, "run_multi", &opt);
    CHECK(m_rx != NULL, "open SHM for multi");

    aio = xlink_aio_create(0);
    CHECK(aio != NULL, "multi aio engine");

    cb_ctx_t mctx = {0, -1, 5, 1};  /* drain=1 — consume each msg */

    /* Start sender thread — shm_writen blocks if no consumer,
     * so the sender must run concurrently with xlink_run(). */
    pthread_t mthr;
    thread_args_t mta = {"run_multi", 0};
    pthread_create(&mthr, NULL, delayed_multi_sender, &mta);

    rc = xlink_run(&m_rx, 1, 5000, aio, count_cb, &mctx);
    pthread_join(mthr, NULL);

    CHECK(rc == 0, "xlink_run returns 0 after 5 events");
    CHECK(mctx.count == 5, "callback called 5 times");

    xlink_aio_destroy(aio);
    xlink_close(m_rx);

    /* ─── Timeout ─── */
    printf("\n--- Timeout ---\n");

    xlink_channel_t *t_rx = xlink_open(XLINK_SHM, "run_timeout", &opt);
    CHECK(t_rx != NULL, "open SHM for timeout test");

    aio = xlink_aio_create(0);
    CHECK(aio != NULL, "timeout aio engine");

    int called = 0;
    rc = xlink_run(&t_rx, 1, 500, aio, timeout_test_cb, &called);
    CHECK(rc == -1, "xlink_run returns -1 on timeout");
    CHECK(called == 0, "callback was NOT called (timeout)");

    xlink_aio_destroy(aio);
    xlink_close(t_rx);

    /* ─── NULL aio (auto-create) ─── */
    printf("\n--- NULL engine (auto-create) ---\n");

    xlink_channel_t *a_rx = xlink_open(XLINK_SHM, "run_auto", &opt);
    CHECK(a_rx != NULL, "open SHM for auto-test");

    thread_args_t ata = {"run_auto", 50};
    pthread_create(&thr, NULL, delayed_sender, &ata);

    cb_ctx_t actx = {0, -1, 1, 0};
    rc = xlink_run(&a_rx, 1, 2000, NULL, count_cb, &actx);  /* NULL aio */
    pthread_join(thr, NULL);

    CHECK(rc == 0, "xlink_run with NULL aio returns 0");
    CHECK(actx.count == 1, "auto-created engine: callback called once");
    xlink_close(a_rx);

    /* ─── Error: NULL channels ─── */
    printf("\n--- Error cases ---\n");
    CHECK(xlink_run(NULL, 1, 100, NULL, count_cb, NULL) == -2, "NULL chans → -2");
    CHECK(xlink_run(&rx, 0, 100, NULL, count_cb, NULL) == -2, "n=0 → -2");
    CHECK(xlink_run(&rx, 1, 100, NULL, NULL, NULL) == -2, "NULL cb → -2");

    printf("\n=== RESULTS: %d checks, %d failures ===\n", npass, nfail);
    return nfail > 0 ? 1 : 0;
}