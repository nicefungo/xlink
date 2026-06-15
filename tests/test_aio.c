/* test_aio.c — async I/O engine tests */

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
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

int main(void) {
    printf("=== Async I/O engine tests ===\n\n");

    /* ─── Engine creation ─── */
    printf("--- Engine creation ---\n");

    void *aio = xlink_aio_create(0);  /* AUTO */
    CHECK(aio != NULL, "xlink_aio_create(AUTO) returns non-NULL");

    const char *name = xlink_aio_name(aio);
    CHECK(name != NULL, "xlink_aio_name() returns non-NULL");
    printf("       engine = '%s'\n", name);

    xlink_aio_destroy(aio);

    /* Explicit poll engine */
    aio = xlink_aio_create(1);  /* POLL */
    CHECK(aio != NULL, "xlink_aio_create(POLL) returns non-NULL");
    name = xlink_aio_name(aio);
    CHECK(strcmp(name, "poll") == 0, "POLL engine name is 'poll'");
    xlink_aio_destroy(aio);

    /* NULL destroy is safe */
    xlink_aio_destroy(NULL);

    /* ─── xlink_wait_aio() with pipe ─── */
    printf("\n--- xlink_wait_aio() with pipe ---\n");

    aio = xlink_aio_create(0);

    xlink_opt_t opt = {
        .flags      = XLINK_CREATE,
        .buf_size   = 0,
        .timeout_ms = -1,
        .shm        = {0}
    };

    xlink_channel_t *pipe_ch = xlink_open(XLINK_PIPE, "/tmp/test_aio_pipe", &opt);
    CHECK(pipe_ch != NULL, "pipe open for aio test");

    /* Timeout test — no data, should timeout */
    xlink_channel_t *chans[1] = { pipe_ch };
    int rc = xlink_wait_aio(chans, 1, 100, aio);
    CHECK(rc == -1, "wait_aio() returns -1 on timeout");

    /* Zero timeout — poll once, no data */
    rc = xlink_wait_aio(chans, 1, 0, aio);
    CHECK(rc == -1, "wait_aio(timeout=0) returns -1 with no data");

    /* Send data, then wait — should return index 0 */
    const char *msg = "aio_test";
    rc = xlink_send(pipe_ch, msg, strlen(msg) + 1);
    CHECK(rc == 0, "pipe send succeeds");
    if (rc == 0) {
        rc = xlink_wait_aio(chans, 1, 500, aio);
        CHECK(rc == 0, "wait_aio() returns index 0 after send");
    }

    /* Consume the data */
    size_t len = 256;
    char buf[256];
    rc = xlink_recv(pipe_ch, buf, &len);
    CHECK(rc == 0, "pipe recv succeeds");
    CHECK(strcmp(buf, "aio_test") == 0, "pipe content matches");

    xlink_close(pipe_ch);

    /* ─── xlink_wait_aio() with SHM ─── */
    printf("\n--- xlink_wait_aio() with SHM ---\n");

    xlink_channel_t *shm_tx = xlink_open(XLINK_SHM, "test_aio_shm", &opt);
    xlink_channel_t *shm_rx = xlink_open(XLINK_SHM, "test_aio_shm",
                                         &XLINK_OPT_DEFAULT);
    CHECK(shm_tx != NULL, "SHM tx open");
    CHECK(shm_rx != NULL, "SHM rx open");

    xlink_channel_t *mix[2] = { shm_tx, shm_rx };

    /* Timeout — no data on SHM */
    rc = xlink_wait_aio(mix, 2, 100, aio);
    CHECK(rc == -1, "SHM wait_aio() returns -1 on timeout");

    /* Send SHM data, wait with aio.
     * Note: both tx and rx share the same SHM channel name,
     * so peek on either returns data.  We accept either index. */
    rc = xlink_send(shm_tx, "shm_aio", 8);
    CHECK(rc == 0, "SHM send succeeds");
    if (rc == 0) {
        rc = xlink_wait_aio(mix, 2, 500, aio);
        CHECK(rc == 0 || rc == 1, "SHM wait_aio() returns valid index (0 or 1)");
    }

    /* Consume from rx */
    len = 256;
    rc = xlink_recv(shm_rx, buf, &len);
    CHECK(rc == 0, "SHM recv succeeds");
    CHECK(strcmp(buf, "shm_aio") == 0, "SHM content matches");

    xlink_close(shm_tx);
    xlink_close(shm_rx);

    /* ─── SHM FIFO notification verification ─── */
    printf("\n--- SHM FIFO notification ---\n");

    shm_tx = xlink_open(XLINK_SHM, "test_evfd", &opt);
    shm_rx = xlink_open(XLINK_SHM, "test_evfd", &XLINK_OPT_DEFAULT);

    CHECK(shm_tx != NULL, "fifo: SHM tx open");
    CHECK(shm_rx != NULL, "fifo: SHM rx open");

    /* Verify FIFO wiring: xlink_wait_aio with SHM-only
     * should work with short timeout (no polling loop). */
    {
        xlink_channel_t *ev_chans[1] = { shm_rx };
        rc = xlink_wait_aio(ev_chans, 1, 50, aio);
        CHECK(rc == -1, "fifo: timeout on empty SHM (no polling)");
    }

    /* After send, wait_aio should return via FIFO notification */
    xlink_send(shm_tx, "evfd_data", 10);
    {
        xlink_channel_t *ev_chans[1] = { shm_rx };
        rc = xlink_wait_aio(ev_chans, 1, 500, aio);
        CHECK(rc == 0, "fifo: wake on data — wait_aio returns index 0");
        len = 256;
        xlink_recv(shm_rx, buf, &len);
        CHECK(strcmp(buf, "evfd_data") == 0, "fifo: data matches");
    }

    /* Multi-send: each send must be consumed before next send
     * (shm_ipc broadcast mode: shm_writen blocks until all readers
     *  consume previous message). */
    xlink_send(shm_tx, "m1", 3);
    {
        xlink_channel_t *ev_chans[1] = { shm_rx };
        rc = xlink_wait_aio(ev_chans, 1, 500, aio);
        CHECK(rc == 0, "fifo: multi-send m1 wakes");
        len = 256;
        xlink_recv(shm_rx, buf, &len);
        CHECK(strcmp(buf, "m1") == 0, "fifo: first msg 'm1'");
    }
    xlink_send(shm_tx, "m2", 3);
    {
        xlink_channel_t *ev_chans[1] = { shm_rx };
        rc = xlink_wait_aio(ev_chans, 1, 500, aio);
        CHECK(rc == 0, "fifo: multi-send m2 wakes");
        len = 256;
        xlink_recv(shm_rx, buf, &len);
        CHECK(strcmp(buf, "m2") == 0, "fifo: second msg 'm2'");
    }

    xlink_close(shm_tx);
    xlink_close(shm_rx);

    /* ─── Mixed SHM + Pipe wait ─── */
    printf("\n--- Mixed SHM + Pipe wait ───\n");

    pipe_ch = xlink_open(XLINK_PIPE, "/tmp/test_aio_mix", &opt);
    shm_tx  = xlink_open(XLINK_SHM,  "test_aio_mix", &opt);
    shm_rx  = xlink_open(XLINK_SHM,  "test_aio_mix", &XLINK_OPT_DEFAULT);

    CHECK(pipe_ch != NULL, "mixed: pipe open");
    CHECK(shm_tx != NULL, "mixed: SHM tx open");
    CHECK(shm_rx != NULL, "mixed: SHM rx open");

    xlink_channel_t *mixed[3] = { pipe_ch, shm_tx, shm_rx };

    /* Send on SHM only, pipe empty → should get SHM (index 1 or 2) */
    xlink_send(shm_tx, "shm_first", 10);
    rc = xlink_wait_aio(mixed, 3, 500, aio);
    CHECK(rc == 1 || rc == 2, "mixed wait returns SHM index (1 or 2)");
    len = 256;
    xlink_recv(shm_rx, buf, &len);
    CHECK(strcmp(buf, "shm_first") == 0, "SHM data from mixed wait matches");

    /* Send on pipe → should get pipe index (0) */
    xlink_send(pipe_ch, "pipe_data", 10);
    rc = xlink_wait_aio(mixed, 3, 500, aio);
    CHECK(rc == 0, "mixed wait returns pipe index (0)");
    len = 256;
    xlink_recv(pipe_ch, buf, &len);
    CHECK(strcmp(buf, "pipe_data") == 0, "pipe data from mixed wait matches");

    xlink_close(pipe_ch);
    xlink_close(shm_tx);
    xlink_close(shm_rx);

    xlink_aio_destroy(aio);

    /* ─── NULL aio → creates default ─── */
    printf("\n--- Default engine ---\n");
    aio = xlink_aio_create(0);
    CHECK(aio != NULL, "default engine created");
    xlink_aio_destroy(aio);

    printf("\n=== RESULTS: %d checks, %d failures ===\n", npass + nfail, nfail);
    return nfail > 0 ? 1 : 0;
}
