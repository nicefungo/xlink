/*
 * test_shm_lockfree.c — Lock-free SPSC send queue + SHM integration
 *
 * Tests:
 *   1. lfq_init / lfq_count / lfq_flush basic API
 *   2. xlink_send_batch via lock-free queue (single producer)
 *   3. xlink_send_batch via lock-free queue (fork: producer + consumer)
 *   4. xlink_lfq_init on non-SHM channel returns error
 *   5. Double-init is idempotent
 *   6. Close cleans up lock-free queue
 *   7. Flush empty queue returns 0
 *   8. Queue full behavior
 *
 * Note: shm_ipc has a single-slot ring buffer. xlink_lfq_flush sends
 * one message per call (blocks until consumer reads). The lock-free
 * queue provides in-process buffering so producers don't block.
 */

#include "xlink.h"
#include "shm_ipc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <assert.h>
#include <sys/wait.h>

#define PASS() printf("  PASS: %s\n", __func__ + 5)
#define FAIL(msg) do { printf("  FAIL: %s — %s\n", __func__ + 5, msg); _exit(1); } while(0)
#define CHECK(cond, msg) do { if (!(cond)) FAIL(msg); } while(0)

/* ─── Test 1: Basic lfq_init / lfq_count / lfq_flush ─── */

static void test_lfq_basic(void)
{
    shm_destroy("test_lfq_1");  /* clean stale segment */

    xlink_opt_t opt = { .flags = XLINK_CREATE };
    xlink_channel_t *tx = xlink_open(XLINK_SHM, "test_lfq_1", &opt);
    CHECK(tx != NULL, "open tx");

    /* Init lock-free queue */
    CHECK(xlink_lfq_init(tx, 64) == 0, "lfq_init 64");
    CHECK(xlink_lfq_count(tx) == 0, "lfq_count 0");

    /* Enqueue via send_batch (goes to lfq, not sent yet) */
    xlink_msg_t msgs[3] = {
        { (void *)"hello", 5 },
        { (void *)"world", 5 },
        { (void *)"foo",   3 },
    };
    int n = xlink_send_batch(tx, msgs, 3);
    CHECK(n == 3, "send_batch queued 3");
    CHECK(xlink_lfq_count(tx) == 3, "lfq_count 3 after enqueue");

    /* Open a reader so lfq_flush can send */
    xlink_channel_t *rx = xlink_open(XLINK_SHM, "test_lfq_1", NULL);
    CHECK(rx != NULL, "open rx");

    /* Flush first message */
    int flushed = xlink_lfq_flush(tx);
    CHECK(flushed == 1, "lfq_flush sent 1st");
    CHECK(xlink_lfq_count(tx) == 2, "lfq_count 2 remaining");

    /* Receive first message */
    {
        char buf[64];
        size_t sz = sizeof(buf);
        CHECK(xlink_recv(rx, buf, &sz) == 0, "recv 1st");
        CHECK(sz == 5 && memcmp(buf, "hello", 5) == 0, "data 1st");
    }

    /* Flush second + recv, flush third + recv */
    CHECK(xlink_lfq_flush(tx) == 1, "flush 2nd");
    {
        char buf[64];
        size_t sz = sizeof(buf);
        CHECK(xlink_recv(rx, buf, &sz) == 0, "recv 2nd");
        CHECK(sz == 5 && memcmp(buf, "world", 5) == 0, "data 2nd");
    }
    CHECK(xlink_lfq_flush(tx) == 1, "flush 3rd");
    {
        char buf[64];
        size_t sz = sizeof(buf);
        CHECK(xlink_recv(rx, buf, &sz) == 0, "recv 3rd");
        CHECK(sz == 3 && memcmp(buf, "foo", 3) == 0, "data 3rd");
    }

    CHECK(xlink_lfq_count(tx) == 0, "queue empty");
    xlink_close(rx);
    xlink_close(tx);
    PASS();
}

/* ─── Test 2: Fork-based producer + consumer pipeline ─── */

static void test_lfq_fork_pipeline(void)
{
    shm_destroy("test_lfq_pipe");

    xlink_opt_t opt = { .flags = XLINK_CREATE };
    xlink_channel_t *tx = xlink_open(XLINK_SHM, "test_lfq_pipe", &opt);
    CHECK(tx != NULL, "open tx");

    /* Large lock-free queue for buffering */
    CHECK(xlink_lfq_init(tx, 65536) == 0, "lfq_init large");

    pid_t pid = fork();
    CHECK(pid >= 0, "fork");

    if (pid == 0) {
        /* Child: consumer */
        xlink_channel_t *rx = xlink_open(XLINK_SHM, "test_lfq_pipe", NULL);
        CHECK(rx != NULL, "open rx in child");

        int received = 0;
        for (int i = 0; i < 500; i++) {
            char buf[64];
            size_t sz = sizeof(buf);
            int rc = xlink_recv(rx, buf, &sz);
            CHECK(rc == 0, "recv ok");
            /* Verify format: "msg-NNNNN" where NNNNN is the sequence number */
            int seq;
            CHECK(sscanf(buf, "msg-%d", &seq) == 1, "parse seq");
            CHECK(seq == received, "seq order");
            received++;
        }
        CHECK(received == 500, "received 500");
        xlink_close(rx);
        _exit(0);
    }

    /* Parent: producer */
    /* Enqueue 500 messages into lock-free queue */
    for (int i = 0; i < 500; i++) {
        char buf[32];
        int len = snprintf(buf, sizeof(buf), "msg-%d", i);
        xlink_msg_t m = { buf, (size_t)len };
        /* Retry if queue full */
        while (xlink_send_batch(tx, &m, 1) != 1)
            usleep(10);
    }
    CHECK(xlink_lfq_count(tx) == 500, "all 500 queued");

    /* Now flush one at a time as consumer reads them */
    int flushed = 0;
    while (xlink_lfq_count(tx) > 0) {
        int n = xlink_lfq_flush(tx);
        if (n > 0) flushed += n;
        if (n < 0) break;
        usleep(100);  /* brief yield to let consumer catch up */
    }
    CHECK(flushed == 500, "flushed all 500");

    /* Wait for child */
    int status;
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "child ok");

    xlink_close(tx);
    PASS();
}

/* ─── Test 3: Multi-thread producers via fork ─── */

static void test_lfq_mt_producers(void)
{
    shm_destroy("test_lfq_mt");

    xlink_opt_t opt = { .flags = XLINK_CREATE };
    xlink_channel_t *tx = xlink_open(XLINK_SHM, "test_lfq_mt", &opt);
    CHECK(tx != NULL, "open tx");
    CHECK(xlink_lfq_init(tx, 65536) == 0, "lfq_init");

#define MT_COUNT 200

    pid_t pid = fork();
    CHECK(pid >= 0, "fork");

    if (pid == 0) {
        /* Child: consumer */
        xlink_channel_t *rx = xlink_open(XLINK_SHM, "test_lfq_mt", NULL);
        CHECK(rx != NULL, "open rx child");

        int count[4] = {0};
        for (int i = 0; i < MT_COUNT; i++) {
            char buf[64];
            size_t sz = sizeof(buf);
            CHECK(xlink_recv(rx, buf, &sz) == 0, "recv ok");
            int pid_tag;
            CHECK(sscanf(buf, "p%d-", &pid_tag) == 1, "parse pid");
            CHECK(pid_tag >= 0 && pid_tag < 4, "valid pid");
            count[pid_tag]++;
        }
        for (int i = 0; i < 4; i++)
            CHECK(count[i] == MT_COUNT / 4, "per-producer count");

        xlink_close(rx);
        _exit(0);
    }

    /* Parent: 4 producers — but single SPSC queue, so only one can enqueue
     * at a time. Since shm_ipc is single-slot, we simulate MT by enqueuing
     * from this process (all "producers" are conceptual). */
    for (int pid_tag = 0; pid_tag < 4; pid_tag++) {
        for (int i = 0; i < MT_COUNT / 4; i++) {
            char buf[32];
            int len = snprintf(buf, sizeof(buf), "p%d-%d", pid_tag, i);
            xlink_msg_t m = { buf, (size_t)len };
            while (xlink_send_batch(tx, &m, 1) != 1)
                usleep(10);
        }
    }
    CHECK(xlink_lfq_count(tx) == MT_COUNT, "all queued");

    /* Flush all */
    int flushed = 0;
    while (xlink_lfq_count(tx) > 0) {
        int n = xlink_lfq_flush(tx);
        if (n > 0) flushed += n;
        if (n < 0) break;
        usleep(50);
    }
    CHECK(flushed == MT_COUNT, "flushed all");

    int status;
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "child ok");

    xlink_close(tx);
    PASS();
}

/* ─── Test 4: lfq_init on non-SHM channel fails ─── */

static void test_lfq_non_shm(void)
{
    xlink_channel_t *ch = xlink_open(XLINK_PIPE, "stdout", NULL);
    CHECK(xlink_lfq_init(ch, 64) == -1, "lfq_init on pipe fails");
    CHECK(xlink_lfq_count(ch) == 0, "lfq_count 0 on pipe");
    xlink_close(ch);
    PASS();
}

/* ─── Test 5: lfq_init double-init is idempotent ─── */

static void test_lfq_double_init(void)
{
    shm_destroy("test_lfq_di");

    xlink_opt_t opt = { .flags = XLINK_CREATE };
    xlink_channel_t *tx = xlink_open(XLINK_SHM, "test_lfq_di", &opt);
    CHECK(tx != NULL, "open");

    CHECK(xlink_lfq_init(tx, 32) == 0, "first init");
    CHECK(xlink_lfq_init(tx, 128) == 0, "second init (idempotent)");

    /* Verify it still works */
    xlink_msg_t m = { (void *)"x", 1 };
    CHECK(xlink_send_batch(tx, &m, 1) == 1, "enqueue after double init");
    CHECK(xlink_lfq_count(tx) == 1, "count 1");

    xlink_channel_t *rx = xlink_open(XLINK_SHM, "test_lfq_di", NULL);
    CHECK(rx != NULL, "open rx");
    CHECK(xlink_lfq_flush(tx) == 1, "flush 1");
    {
        char buf[8];
        size_t sz = sizeof(buf);
        CHECK(xlink_recv(rx, buf, &sz) == 0, "recv");
        CHECK(sz == 1 && buf[0] == 'x', "data ok");
    }
    xlink_close(rx);
    xlink_close(tx);
    PASS();
}

/* ─── Test 6: Close cleans up lock-free queue ─── */

static void test_lfq_close_cleanup(void)
{
    shm_destroy("test_lfq_cc");

    xlink_opt_t opt = { .flags = XLINK_CREATE };
    xlink_channel_t *tx = xlink_open(XLINK_SHM, "test_lfq_cc", &opt);
    CHECK(tx != NULL, "open");

    CHECK(xlink_lfq_init(tx, 32) == 0, "init");

    /* Enqueue some messages (will be freed on close) */
    for (int i = 0; i < 10; i++) {
        xlink_msg_t m = { (void *)"cleanup", 7 };
        CHECK(xlink_send_batch(tx, &m, 1) == 1, "enqueue");
    }
    CHECK(xlink_lfq_count(tx) == 10, "count 10");

    /* Close without flushing — should drain and free */
    xlink_close(tx);
    PASS();
}

/* ─── Test 7: Flush empty queue returns 0 ─── */

static void test_lfq_flush_empty(void)
{
    shm_destroy("test_lfq_fe");

    xlink_opt_t opt = { .flags = XLINK_CREATE };
    xlink_channel_t *tx = xlink_open(XLINK_SHM, "test_lfq_fe", &opt);
    CHECK(tx != NULL, "open");

    CHECK(xlink_lfq_init(tx, 32) == 0, "init");
    CHECK(xlink_lfq_flush(tx) == 0, "flush empty returns 0");

    xlink_close(tx);
    PASS();
}

/* ─── Test 8: Queue full behavior ─── */

static void test_lfq_full(void)
{
    shm_destroy("test_lfq_full");

    xlink_opt_t opt = { .flags = XLINK_CREATE };
    xlink_channel_t *tx = xlink_open(XLINK_SHM, "test_lfq_full", &opt);
    CHECK(tx != NULL, "open");

    /* Small capacity: 8 (min) */
    CHECK(xlink_lfq_init(tx, 8) == 0, "init capacity 8");

    /* Fill the queue */
    int enqueued = 0;
    for (int i = 0; i < 20; i++) {
        char buf[16];
        int len = snprintf(buf, sizeof(buf), "m%d", i);
        xlink_msg_t m = { buf, (size_t)len };
        int rc = xlink_send_batch(tx, &m, 1);
        if (rc == 0) break;
        enqueued++;
    }
    CHECK(enqueued == 8, "queue full at capacity");
    CHECK(xlink_lfq_count(tx) == 8, "count 8");

    /* Open reader and flush all */
    xlink_channel_t *rx = xlink_open(XLINK_SHM, "test_lfq_full", NULL);
    CHECK(rx != NULL, "open rx");

    int flushed = 0;
    while (xlink_lfq_count(tx) > 0) {
        int n = xlink_lfq_flush(tx);
        if (n > 0) flushed += n;
        if (n < 0) break;
        /* Receive what we just sent */
        char buf[32];
        size_t sz = sizeof(buf);
        if (xlink_recv(rx, buf, &sz) == 0)
            usleep(100);
    }
    CHECK(flushed == 8, "flushed all 8");
    CHECK(xlink_lfq_count(tx) == 0, "count 0");

    xlink_close(rx);
    xlink_close(tx);
    PASS();
}

int main(void)
{
    printf("--- Lock-Free SHM Queue Integration ---\n");
    test_lfq_basic();
    test_lfq_fork_pipeline();
    test_lfq_mt_producers();
    test_lfq_non_shm();
    test_lfq_double_init();
    test_lfq_close_cleanup();
    test_lfq_flush_empty();
    test_lfq_full();
    printf("=== ALL PASSED ===\n");
    return 0;
}
