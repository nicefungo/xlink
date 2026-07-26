/*
 * test_zc_notify.c — test zero-copy completion notification (eventfd)
 * Step 2.3: SHM completion notification (eventfd/FIFO integration)
 */

#include "xlink.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <assert.h>
#include <sys/wait.h>

static int n_pass = 0, n_fail = 0;
#define CHECK(cond, desc) do { \
    if (cond) { fprintf(stderr, "  PASS [%d]: %s\n", ++n_pass, desc); } \
    else { fprintf(stderr, "  FAIL [%d]: %s\n", ++n_fail, desc); } \
} while(0)

static int cb_count;
static void my_cb(xlink_channel_t *ch, uint64_t tag, int status, void *ud) {
    (void)ch; (void)tag; (void)status; (void)ud;
    cb_count++;
}

int main(void) {
    fprintf(stderr, "\n=== Zero-Copy Completion Notification (Step 2.3) ===\n\n");

    const char *msg = "notify-test-data";
    size_t msglen = strlen(msg);

    /* ── Test 1: notify_fd on SHM channel ── */
    fprintf(stderr, "--- notify_fd basic ---\n");
    xlink_channel_t *tx = xlink_open(XLINK_SHM, "zc_n1",
        &(xlink_opt_t){ .flags = XLINK_CREATE | XLINK_SPSC, .buf_size = 65536 });
    CHECK(tx != NULL, "open SHM SPSC for notify test");

    int nfd = xlink_zc_notify_fd(tx);
    CHECK(nfd >= 0, "xlink_zc_notify_fd returns valid fd");

    /* ── Test 2: not readable before send ── */
    fprintf(stderr, "\n--- idle check ---\n");
    {
        struct pollfd pfd = { .fd = nfd, .events = POLLIN };
        int rc = poll(&pfd, 1, 0);
        CHECK(rc == 0, "eventfd not readable before send_zc");
    }

    /* ── Test 3: readable after send_zc ── */
    fprintf(stderr, "\n--- send_zc + poll ---\n");
    {
        xlink_zc_buf_t buf = { .addr = (void *)msg, .len = msglen, .tag = 42 };
        cb_count = 0;
        int rc = xlink_send_zc(tx, &buf, my_cb, NULL);
        CHECK(rc == 0, "send_zc returns 0");
        CHECK(cb_count == 1, "callback fired");

        struct pollfd pfd = { .fd = nfd, .events = POLLIN };
        rc = poll(&pfd, 1, 200);
        CHECK(rc == 1, "eventfd readable after send_zc");
        CHECK(pfd.revents & POLLIN, "POLLIN flag set");
    }

    /* ── Test 4: zc_poll drains ── */
    fprintf(stderr, "\n--- zc_poll drain ---\n");
    {
        int cnt = xlink_zc_poll(tx);
        CHECK(cnt == 1, "zc_poll returns 1");

        struct pollfd pfd = { .fd = nfd, .events = POLLIN };
        int rc = poll(&pfd, 1, 0);
        CHECK(rc == 0, "eventfd NOT readable after drain");
    }

    /* ── Test 5: multi-send, 1 readable ── */
    fprintf(stderr, "\n--- multi-send ---\n");
    {
        xlink_zc_buf_t b = { .addr = (void *)msg, .len = msglen };
        CHECK(xlink_send_zc(tx, &b, NULL, NULL) == 0, "send 1/3");
        CHECK(xlink_send_zc(tx, &b, NULL, NULL) == 0, "send 2/3");
        CHECK(xlink_send_zc(tx, &b, NULL, NULL) == 0, "send 3/3");

        struct pollfd pfd = { .fd = nfd, .events = POLLIN };
        int rc = poll(&pfd, 1, 200);
        CHECK(rc == 1, "eventfd readable after 3 sends");

        int cnt = xlink_zc_poll(tx);
        CHECK(cnt == 3, "zc_poll returns 3");
    }

    /* ── Test 6: pipe channel notify_fd ── */
    fprintf(stderr, "\n--- pipe channel ---\n");
    {
        xlink_channel_t *pc = xlink_open(XLINK_PIPE, "/tmp/zc_n_pipe",
            &(xlink_opt_t){ .flags = XLINK_CREATE });
        CHECK(pc != NULL, "open pipe");
        int pfd = xlink_zc_notify_fd(pc);
        /* pipe may or may not support notify_fd — both are fine */
        fprintf(stderr, "  INFO: pipe notify_fd=%d\n", pfd);
        xlink_close(pc);
        unlink("/tmp/zc_n_pipe");
    }

    /* ── Test 7: fork + poll + zc_poll ── */
    fprintf(stderr, "\n--- fork scenario ---\n");
    {
        pid_t pid = fork();
        if (pid == 0) {
            /* child: open, send_zc, zc_poll, close */
            sleep(1);
            xlink_channel_t *ct = xlink_open(XLINK_SHM, "zc_n_fork",
                &(xlink_opt_t){ .flags = XLINK_CREATE | XLINK_SPSC,
                                .buf_size = 65536 });
            assert(ct);
            xlink_zc_buf_t b = { .addr = (void *)"chmsg", .len = 6, .tag = 99 };
            int r = xlink_send_zc(ct, &b, NULL, NULL);
            assert(r == 0);
            int c = xlink_zc_poll(ct);
            assert(c == 1);
            xlink_close(ct);
            _exit(0);
        }

        /* parent */
        xlink_channel_t *ptx = xlink_open(XLINK_SHM, "zc_n_fork",
            &(xlink_opt_t){ .flags = XLINK_CREATE | XLINK_SPSC,
                            .buf_size = 65536 });
        CHECK(ptx != NULL, "parent open OK");
        int pfd = xlink_zc_notify_fd(ptx);
        CHECK(pfd >= 0, "parent notify_fd OK");

        /* send a msg in parent and wait */
        xlink_zc_buf_t b = { .addr = (void *)"parent", .len = 7, .tag = 100 };
        CHECK(xlink_send_zc(ptx, &b, NULL, NULL) == 0, "parent send_zc OK");

        struct pollfd pollfd = { .fd = pfd, .events = POLLIN };
        int rc = poll(&pollfd, 1, 400);
        CHECK(rc == 1, "parent poll wakes on zc completion");

        int cnt = xlink_zc_poll(ptx);
        CHECK(cnt >= 1, "parent zc_poll returns completions");

        int status;
        waitpid(pid, &status, 0);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "child exit 0");

        xlink_close(ptx);
    }

    /* cleanup */
    xlink_close(tx);

    fprintf(stderr, "\n=== RESULTS: %d checks, %d failures ===\n\n",
            n_pass + n_fail, n_fail);
    return n_fail ? 1 : 0;
}
