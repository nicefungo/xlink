/*
 * test_shm_spsc_channel.c — xlink SHM channel with XLINK_SPSC flag
 *
 * Validates the lock-free SPSC shared-memory path through xlink's
 * public API (xlink_open → xlink_send → xlink_recv → xlink_close).
 *
 * Tests:
 *   1. Basic send/recv via SPSC channel (single-process)
 *   2. Multiple messages with various sizes
 *   3. Queue full → error behavior
 *   4. Non-CREATE open detection (auto-detect SPSC region)
 *   5. Fork: parent produces, child consumes
 */

#include "xlink.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

static int failed = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        printf("  FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
        failed = 1; \
    } else { \
        printf("  PASS: %s\n", msg); \
    } \
} while (0)

/* ─── Test 1: Basic SPSC send/recv ─── */
static void test_spsc_basic(void)
{
    printf("\n--- Test 1: SPSC basic send/recv ---\n");

    xlink_opt_t opt = { .flags = XLINK_CREATE | XLINK_SPSC };
    xlink_channel_t *ch = xlink_open(XLINK_SHM, "ts_spsc_basic", &opt);
    CHECK(ch != NULL, "open SPSC channel");

    const char *msg = "hello spsc";
    int rc = xlink_send(ch, msg, strlen(msg));
    CHECK(rc == 0, "send 'hello spsc'");

    char buf[256] = {0};
    size_t len = sizeof(buf);
    rc = xlink_recv(ch, buf, &len);
    CHECK(rc == 0, "recv OK");
    CHECK(len == strlen(msg), "recv length correct");
    CHECK(strcmp(buf, msg) == 0, "recv content correct");

    xlink_close(ch);
}

/* ─── Test 2: Multiple messages, various sizes ─── */
static void test_spsc_multi(void)
{
    printf("\n--- Test 2: SPSC multi-message ---\n");

    xlink_opt_t opt = { .flags = XLINK_CREATE | XLINK_SPSC };
    xlink_channel_t *ch = xlink_open(XLINK_SHM, "ts_spsc_multi", &opt);
    CHECK(ch != NULL, "open SPSC channel");

    const char *msgs[] = {
        "short",
        "a medium length message for testing",
        "x",
        "a bit longer message that tests the ring buffer wrapping behavior"
    };
    int n = sizeof(msgs)/sizeof(msgs[0]);

    for (int i = 0; i < n; i++) {
        int rc = xlink_send(ch, msgs[i], strlen(msgs[i]));
        CHECK(rc == 0, "send msg");
    }

    for (int i = 0; i < n; i++) {
        char buf[256] = {0};
        size_t len = sizeof(buf);
        int rc = xlink_recv(ch, buf, &len);
        CHECK(rc == 0, "recv msg");
        CHECK(len == strlen(msgs[i]), "recv length");
        CHECK(strcmp(buf, msgs[i]) == 0, "recv content");
    }

    xlink_close(ch);
}

/* ─── Test 3: Non-CREATE open auto-detects SPSC ─── */
static void test_spsc_autodetect(void)
{
    printf("\n--- Test 3: SPSC auto-detect on non-CREATE open ---\n");

    /* First, create the SPSC channel */
    xlink_opt_t opt_create = { .flags = XLINK_CREATE | XLINK_SPSC };
    xlink_channel_t *tx = xlink_open(XLINK_SHM, "ts_spsc_auto", &opt_create);
    CHECK(tx != NULL, "create SPSC channel");

    /* Open without flags — should auto-detect SPSC region */
    xlink_opt_t opt_default = XLINK_OPT_DEFAULT;
    xlink_channel_t *rx = xlink_open(XLINK_SHM, "ts_spsc_auto", &opt_default);
    CHECK(rx != NULL, "open without SPSC flag");

    const char *msg = "autodetect";
    int rc = xlink_send(tx, msg, strlen(msg));
    CHECK(rc == 0, "send via SPSC");

    char buf[256] = {0};
    size_t len = sizeof(buf);
    rc = xlink_recv(rx, buf, &len);
    CHECK(rc == 0, "recv via SPSC autodetect");
    CHECK(strcmp(buf, msg) == 0, "content match");

    xlink_close(tx);
    xlink_close(rx);
}

/* ─── Test 4: Fork: parent produces, child consumes ─── */
static void test_spsc_fork(void)
{
    printf("\n--- Test 4: SPSC fork (parent→child) ---\n");

    xlink_opt_t opt = { .flags = XLINK_CREATE | XLINK_SPSC };
    xlink_channel_t *tx = xlink_open(XLINK_SHM, "ts_spsc_fork", &opt);
    CHECK(tx != NULL, "open SPSC tx");

    pid_t pid = fork();
    CHECK(pid >= 0, "fork");

    if (pid == 0) {
        /* Child: open as consumer */
        xlink_opt_t rx_opt = XLINK_OPT_DEFAULT;
        xlink_channel_t *rx = xlink_open(XLINK_SHM, "ts_spsc_fork", &rx_opt);
        if (!rx) {
            fprintf(stderr, "child: open failed\n");
            _exit(1);
        }

        const char *expected[] = {"msg1", "msg2", "msg3"};
        for (int i = 0; i < 3; i++) {
            char buf[256] = {0};
            size_t len = sizeof(buf);
            int rc;
            /* Poll with small timeout */
            for (int retry = 0; retry < 1000; retry++) {
                rc = xlink_recv(rx, buf, &len);
                if (rc == 0) break;
                usleep(1000);
            }
            if (rc != 0) {
                fprintf(stderr, "child: recv msg %d failed\n", i);
                xlink_close(rx);
                _exit(1);
            }
            if (strcmp(buf, expected[i]) != 0) {
                fprintf(stderr, "child: msg %d mismatch: '%s' vs '%s'\n",
                        i, buf, expected[i]);
                xlink_close(rx);
                _exit(1);
            }
        }
        xlink_close(rx);
        _exit(0);
    }

    /* Parent: send messages */
    const char *msgs[] = {"msg1", "msg2", "msg3"};
    for (int i = 0; i < 3; i++) {
        int rc = xlink_send(tx, msgs[i], strlen(msgs[i]));
        CHECK(rc == 0, "parent send");
        usleep(5000);  /* brief pause between messages */
    }

    int status;
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "child exit 0");

    xlink_close(tx);
}

/* ─── Test 5: Large message near ring boundary ─── */
static void test_spsc_boundary(void)
{
    printf("\n--- Test 5: SPSC ring boundary/wrap test ---\n");

    xlink_opt_t opt = { .flags = XLINK_CREATE | XLINK_SPSC };
    xlink_channel_t *ch = xlink_open(XLINK_SHM, "ts_spsc_boundary", &opt);
    CHECK(ch != NULL, "open SPSC channel");

    /* Send enough messages to fill a significant portion of the ring */
    char msgbuf[4096];
    memset(msgbuf, 'A', sizeof(msgbuf));
    int sent = 0;
    for (int i = 0; i < 20; i++) {
        msgbuf[0] = 'A' + (i % 26);
        if (xlink_send(ch, msgbuf, sizeof(msgbuf)) == 0) {
            sent++;
        } else {
            break; /* ring full */
        }
    }
    CHECK(sent > 5, "sent multiple large messages");

    /* Receive them all back */
    int recvd = 0;
    for (int i = 0; i < sent; i++) {
        char buf[5000] = {0};
        size_t len = sizeof(buf);
        int rc = xlink_recv(ch, buf, &len);
        if (rc == 0) {
            CHECK(len == 4096, "large msg length");
            recvd++;
        } else {
            printf("  NOTE: recv %d failed (ring drained?)\n", i);
            break;
        }
    }
    CHECK(recvd == sent, "all sent messages received");

    xlink_close(ch);
}

int main(void)
{
    printf("=== test_shm_spsc_channel ===\n");

    test_spsc_basic();
    test_spsc_multi();
    test_spsc_autodetect();
    test_spsc_fork();
    test_spsc_boundary();

    printf("\n=== %s ===\n", failed ? "SOME TESTS FAILED" : "ALL PASSED");
    return failed ? 1 : 0;
}
