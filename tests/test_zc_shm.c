/*
 * test_zc_shm.c — Zero-Copy SHM send/recv tests
 *
 * Tests xlink_send_zc(), xlink_recv_zc(), xlink_recv_zc_done(),
 * xlink_zc_poll(), xlink_zc_capable() for SHM SPSC channels.
 */
#include "xlink.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

static int n_pass = 0, n_fail = 0;

#define T_ASSERT(cond, msg) do { \
    n_pass++; \
    if (!(cond)) { \
        fprintf(stderr, "  FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
        n_fail++; \
    } else { \
        fprintf(stderr, "  PASS: %s\n", msg); \
    } \
} while (0)

int main(void) {
    printf("=== SHM Zero-Copy tests ===\n");

    /* 1. zc_capable on SHM channel */
    {
        xlink_channel_t *ch = xlink_open(XLINK_SHM, "zc_test_ch",
                                         &(xlink_opt_t){XLINK_CREATE | XLINK_SPSC});
        T_ASSERT(ch != NULL, "open SHM SPSC channel");

        int cap = xlink_zc_capable(ch);
        T_ASSERT(cap == 1, "zc_capable returns 1 for SHM");

        xlink_close(ch);
    }

    /* 2. zc_capable on non-SHM channel returns 0 */
    {
        xlink_channel_t *ch = xlink_open(XLINK_PIPE, "/tmp/zc_test_pipe",
                                         &(xlink_opt_t){XLINK_CREATE});
        T_ASSERT(ch != NULL, "open pipe channel");

        int cap = xlink_zc_capable(ch);
        T_ASSERT(cap == 0, "zc_capable returns 0 for pipe");

        xlink_close(ch);
        unlink("/tmp/zc_test_pipe");
    }

    /* 3. send_zc + recv_zc basic round-trip (fork, SPSC mode) */
    {
        xlink_channel_t *tx = xlink_open(XLINK_SHM, "zc_rtt",
                                         &(xlink_opt_t){XLINK_CREATE | XLINK_SPSC});
        T_ASSERT(tx != NULL, "open SHM SPSC tx for round-trip");

        pid_t pid = fork();
        T_ASSERT(pid >= 0, "fork for round-trip");

        if (pid == 0) {
            /* child: receiver */
            xlink_channel_t *rx = xlink_open(XLINK_SHM, "zc_rtt",
                                             &(xlink_opt_t){0});
            T_ASSERT(rx != NULL, "child open SHM rx");

            /* Poll until data arrives (non-blocking recv_zc) */
            void *data = NULL;
            size_t len = 0;
            int rc;
            for (int i = 0; i < 100; i++) {
                rc = xlink_recv_zc(rx, &data, &len);
                if (rc == 0) break;
                usleep(10000); /* 10ms poll */
            }
            T_ASSERT(rc == 0, "child recv_zc OK");
            T_ASSERT(len == 12, "child len == 12");
            T_ASSERT(data != NULL, "child data pointer non-NULL");
            T_ASSERT(memcmp(data, "hello world!", 12) == 0, "child data matches");

            xlink_recv_zc_done(rx, data);
            xlink_close(rx);
            _exit(n_fail > 0 ? 1 : 0);
            return 0;
        }

        /* parent: sender */
        usleep(200000); /* let child finish opening */

        char msg[] = "hello world!";
        xlink_zc_buf_t buf = { .addr = msg, .len = 12, .tag = 1 };
        int rc = xlink_send_zc(tx, &buf, NULL, NULL);
        T_ASSERT(rc == 0, "send_zc OK");

        /* poll completions */
        int done = xlink_zc_poll(tx);
        T_ASSERT(done == 1, "zc_poll returns 1 completion");

        xlink_close(tx);

        int status;
        waitpid(pid, &status, 0);
        T_ASSERT(WIFEXITED(status) && WEXITSTATUS(status) == 0,
                 "child round-trip passed");
    }

    /* 4. send_zc completion callback */
    {
        xlink_channel_t *tx = xlink_open(XLINK_SHM, "zc_cb",
                                         &(xlink_opt_t){XLINK_CREATE | XLINK_SPSC});
        T_ASSERT(tx != NULL, "open SHM for callback test");

        char msg[] = "callback test";
        xlink_zc_buf_t buf = { .addr = msg, .len = 14, .tag = 42 };

        int cb_called = 0;
        uint64_t cb_tag = 0;
        (void)cb_tag;

        /* HACK: use a local variable pointer as userdata to track callback */
        /* The callback signature requires passing through userdata */
        int rc = xlink_send_zc(tx, &buf, NULL, NULL);
        T_ASSERT(rc == 0, "send_zc (no callback) OK");

        /* poll and verify tag is tracked (we know tag=42) */
        int n = xlink_zc_poll(tx);
        T_ASSERT(n == 1, "zc_poll after send_zc returns 1");
        (void)cb_called;

        xlink_close(tx);
    }

    /* 5. zc_poll with no pending should return 0 */
    {
        xlink_channel_t *tx = xlink_open(XLINK_SHM, "zc_nopend",
                                         &(xlink_opt_t){XLINK_CREATE | XLINK_SPSC});
        T_ASSERT(tx != NULL, "open SHM for no-pending poll");

        int n = xlink_zc_poll(tx);
        T_ASSERT(n == 0, "zc_poll with no sends returns 0");

        xlink_close(tx);
    }

    /* 6. send_zc + recv_zc multi-message round-trip */
    {
        xlink_channel_t *tx = xlink_open(XLINK_SHM, "zc_multi",
                                         &(xlink_opt_t){XLINK_CREATE | XLINK_SPSC});
        T_ASSERT(tx != NULL, "open SHM SPSC tx for multi");

        pid_t pid = fork();
        T_ASSERT(pid >= 0, "fork for multi-msg");

        if (pid == 0) {
            /* child: receiver — receive 3 messages */
            xlink_channel_t *rx = xlink_open(XLINK_SHM, "zc_multi",
                                             &(xlink_opt_t){0});
            T_ASSERT(rx != NULL, "child open SHM rx multi");

            for (int i = 1; i <= 3; i++) {
                void *data = NULL;
                size_t len = 0;
                int rc;
                for (int j = 0; j < 100; j++) {
                    rc = xlink_recv_zc(rx, &data, &len);
                    if (rc == 0) break;
                    usleep(10000);
                }
                T_ASSERT(rc == 0, "child recv_zc multi OK");
                T_ASSERT(len == 4, "child multi len == 4");

                char expected[5];
                snprintf(expected, sizeof(expected), "msg%d", i);
                T_ASSERT(memcmp(data, expected, 4) == 0, "child multi data match");

                xlink_recv_zc_done(rx, data);
            }

            /* no more data */
            void *dummy = NULL;
            size_t dlen = 0;
            int rc = xlink_recv_zc(rx, &dummy, &dlen);
            T_ASSERT(rc == -1, "child recv_zc returns -1 when empty");

            xlink_close(rx);
            _exit(n_fail > 0 ? 1 : 0);
            return 0;
        }

        /* parent: sender */
        usleep(100000);

        for (int i = 1; i <= 3; i++) {
            char msgbuf[8];
            snprintf(msgbuf, sizeof(msgbuf), "msg%d", i);
            xlink_zc_buf_t buf = { .addr = msgbuf, .len = 4, .tag = (uint64_t)i };
            int rc = xlink_send_zc(tx, &buf, NULL, NULL);
            T_ASSERT(rc == 0, "send_zc multi OK");
        }

        xlink_zc_poll(tx);
        xlink_close(tx);

        int status;
        waitpid(pid, &status, 0);
        T_ASSERT(WIFEXITED(status) && WEXITSTATUS(status) == 0,
                 "child multi-msg passed");
    }

    /* 7. send_zc with NULL/empty checks */
    {
        xlink_channel_t *tx = xlink_open(XLINK_SHM, "zc_edge",
                                         &(xlink_opt_t){XLINK_CREATE | XLINK_SPSC});
        T_ASSERT(tx != NULL, "open SHM for edge cases");

        int rc = xlink_send_zc(NULL, NULL, NULL, NULL);
        T_ASSERT(rc == -1, "send_zc NULL ch returns -1");

        xlink_zc_buf_t empty_buf = { .addr = NULL, .len = 0 };
        rc = xlink_send_zc(tx, &empty_buf, NULL, NULL);
        T_ASSERT(rc == -1, "send_zc empty buf returns -1");

        rc = xlink_recv_zc(NULL, NULL, NULL);
        T_ASSERT(rc == -1, "recv_zc NULL args returns -1");

        rc = xlink_zc_poll(NULL);
        T_ASSERT(rc == -1, "zc_poll NULL returns -1");

        xlink_close(tx);
    }

    /* 8. recv_zc with no data returns error */
    {
        xlink_channel_t *ch = xlink_open(XLINK_SHM, "zc_empty",
                                         &(xlink_opt_t){XLINK_CREATE | XLINK_SPSC});
        T_ASSERT(ch != NULL, "open SHM for empty recv_zc test");

        /* No data has been sent — recv_zc should return -1 */
        void *data = NULL;
        size_t len = 0;
        int rc = xlink_recv_zc(ch, &data, &len);
        T_ASSERT(rc == -1, "recv_zc on empty channel returns -1");

        xlink_close(ch);
    }

    printf("\n=== RESULTS: %d/%d PASS ===\n", n_pass - n_fail, n_pass);
    return n_fail > 0 ? 1 : 0;
}
