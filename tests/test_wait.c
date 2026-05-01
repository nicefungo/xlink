/*
 * xlink_wait() tests.
 *
 * 1. Pipe: wait for data from one channel
 * 2. SHM: wait via peek fallback
 * 3. Pipe: timeout
 * 4. Multi-channel: wait on two pipes
 */

#include "xlink.h"
#include "shm_ipc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>

static int failures = 0;
#define CHECK(cond, msg) do {                                    \
    if (!(cond)) {                                               \
        fprintf(stderr, "  FAIL [%d]: %s\n", __LINE__, msg);     \
        failures++;                                              \
    } else {                                                     \
        printf("  PASS: %s\n", msg);                             \
    }                                                            \
} while(0)

static void test_wait_pipe(void) {
    printf("\n--- Wait on pipe ---\n");

    unlink("/tmp/xlink_wait_test");

    /* Open pipe */
    xlink_opt_t opt = XLINK_OPT_DEFAULT;
    opt.flags = XLINK_CREATE;

    xlink_channel_t* ch = xlink_open(XLINK_PIPE, "/tmp/xlink_wait_test", &opt);
    CHECK(ch != NULL, "open pipe");

    if (!ch) return;

    /* Wait with timeout (no data yet → should time out) */
    xlink_channel_t* chans[1] = { ch };
    int rc = xlink_wait(chans, 1, 100);  /* 100ms timeout */
    CHECK(rc == -1, "wait timeout returns -1 (no data)");

    /* Send data, then wait */
    const char* msg = "hello";
    xlink_send(ch, msg, strlen(msg) + 1);

    rc = xlink_wait(chans, 1, 1000);
    CHECK(rc == 0, "wait on pipe with data returns 0");

    /* Actually read the data */
    uint8_t buf[256];
    size_t len = sizeof(buf);
    rc = xlink_recv(ch, buf, &len);
    CHECK(rc == 0 && len == 6 && memcmp(buf, "hello", 6) == 0, "read matched data");

    xlink_close(ch);
    unlink("/tmp/xlink_wait_test");
}

static void test_wait_shm(void) {
    printf("\n--- Wait on SHM ---\n");

    shm_destroy("/xlink_wait_shm_test");

    xlink_opt_t opt = XLINK_OPT_DEFAULT;
    opt.flags = XLINK_CREATE;

    xlink_channel_t* tx = xlink_open(XLINK_SHM, "/xlink_wait_shm_test", &opt);
    CHECK(tx != NULL, "open SHM sender");

    xlink_channel_t* rx = xlink_open(XLINK_SHM, "/xlink_wait_shm_test", NULL);
    CHECK(rx != NULL, "open SHM receiver");

    if (!tx || !rx) {
        if (tx) xlink_close(tx);
        if (rx) xlink_close(rx);
        return;
    }

    /* Timeout: no data yet */
    xlink_channel_t* chans[1] = { rx };
    int rc = xlink_wait(chans, 1, 100);
    CHECK(rc == -1, "shm wait timeout (no data)");

    /* Send data, then wait */
    xlink_send(tx, "hello", 6);
    rc = xlink_wait(chans, 1, 1000);
    CHECK(rc == 0, "shm wait with data returns 0");

    uint8_t buf[256];
    size_t len = sizeof(buf);
    rc = xlink_recv(rx, buf, &len);
    CHECK(rc == 0 && len == 6, "shm read matched");

    xlink_close(tx);
    xlink_close(rx);
    shm_destroy("/xlink_wait_shm_test");
}

static void test_wait_multi(void) {
    printf("\n--- Wait on two pipes ---\n");

    unlink("/tmp/xlink_multi_A");
    unlink("/tmp/xlink_multi_B");

    xlink_opt_t opt = XLINK_OPT_DEFAULT;
    opt.flags = XLINK_CREATE;

    xlink_channel_t* chA = xlink_open(XLINK_PIPE, "/tmp/xlink_multi_A", &opt);
    xlink_channel_t* chB = xlink_open(XLINK_PIPE, "/tmp/xlink_multi_B", &opt);
    CHECK(chA && chB, "open two pipes");

    if (chA && chB) {
        /* Send data to B only */
        xlink_send(chB, "data", 5);

        xlink_channel_t* chans[2] = { chA, chB };
        int rc = xlink_wait(chans, 2, 1000);
        CHECK(rc == 1, "wait on 2 pipes returns ready channel index 1 (B)");

        uint8_t buf[256];
        size_t len = sizeof(buf);
        rc = xlink_recv(chB, buf, &len);
        CHECK(rc == 0 && len == 5, "read from channel B");

        /* Now send to A and verify we wait on it */
        xlink_send(chA, "more", 5);

        rc = xlink_wait(chans, 2, 1000);
        CHECK(rc == 0, "wait on 2 pipes returns index 0 (A)");

        len = sizeof(buf);
        rc = xlink_recv(chA, buf, &len);
        CHECK(rc == 0 && len == 5, "read from channel A");
    }

    if (chA) xlink_close(chA);
    if (chB) xlink_close(chB);
    unlink("/tmp/xlink_multi_A");
    unlink("/tmp/xlink_multi_B");
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);

    printf("=== xlink_wait() tests ===\n");

    test_wait_pipe();
    test_wait_shm();
    test_wait_multi();

    printf("\n=== %s ===\n", failures ? "FAILED" : "ALL PASSED");
    return failures ? 1 : 0;
}
