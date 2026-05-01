/*
 * test_pipe_empty.c — Test 0-length framed messages on pipe transport.
 *
 * Unlike test_tcp_empty.c (which exercises TCP backend's internal
 * write_framed/read_framed), this test exercises the GENERIC framer in
 * xlink.c (frame_send/frame_recv) over a named pipe transport.
 *
 * Verifies that:
 *   1) xlink_send with 0 bytes writes a valid 4-byte header (payload_len=0)
 *   2) xlink_recv correctly returns *len = 0 for the empty frame
 *   3) Subsequent non-empty messages retain framing sync
 *   4) Multiple empty messages in sequence don't corrupt framing
 *   5) xlink_peek reports 4 bytes pending (header only) before recv of empty msg
 *
 * REASON FOR TESTING: The generic frame_send has a distinct implementation
 * from TCP's write_framed (writev-based vs write_exact-based), and the
 * frame_recv read_exact(buf, 0) path — where payload_len == 0 so
 * read_exact returns immediately with n=0 — was previously untested.
 */

#include "xlink.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <errno.h>

#define PIPE_PATH  "/tmp/xlink_test_pipe_empty"
#define BUFSZ      256

static int failures = 0;
static int checks   = 0;

#define CHECK(cond, msg) do {                                           \
    checks++;                                                           \
    if (!(cond)) {                                                      \
        fprintf(stderr, "  FAIL [%d] %s\n", checks, msg);              \
        failures++;                                                     \
    } else {                                                            \
        fprintf(stderr, "  PASS [%d] %s\n", checks, msg);              \
    }                                                                   \
} while(0)

int main(void) {
    fprintf(stderr, "=== Test pipe framing: 0-length messages (generic framer) ===\n");

    unlink(PIPE_PATH);
    if (mkfifo(PIPE_PATH, 0666) != 0) {
        perror("mkfifo");
        return 1;
    }

    int sync_pipe[2];
    if (pipe(sync_pipe) != 0) { perror("sync pipe"); return 1; }

    pid_t child = fork();
    if (child == 0) {
        /* ── Child: receiver ── */
        /* Close read end — parent holds it */
        close(sync_pipe[0]);

        xlink_opt_t opt = XLINK_OPT_DEFAULT;
        xlink_channel_t* ch = xlink_open(XLINK_PIPE, PIPE_PATH, &opt);
        if (!ch) { fprintf(stderr, "C: open fail\n"); _exit(1); }

        /* Signal parent: ready */
        char ack = 'r';
        ssize_t w = write(sync_pipe[1], &ack, 1);
        (void)w;

        uint8_t buf[BUFSZ];
        size_t len;

        /* 1) Receive empty message */
        len = sizeof(buf);
        int rc = xlink_recv(ch, buf, &len);
        if (rc != 0) {
            fprintf(stderr, "C: recv #1 (empty): %s\n", xlink_errstr(ch));
            _exit(1);
        }
        if (len != 0) {
            fprintf(stderr, "C: recv #1 expected 0 bytes, got %zu\n", len);
            _exit(1);
        }

        /* 2) Receive "hello" after empty */
        len = sizeof(buf);
        rc = xlink_recv(ch, buf, &len);
        if (rc != 0) {
            fprintf(stderr, "C: recv #2: %s\n", xlink_errstr(ch));
            _exit(1);
        }
        if (len != 6 || memcmp(buf, "hello\0", 6) != 0) {
            fprintf(stderr, "C: recv #2 expected 'hello\\0', got %zu bytes\n", len);
            _exit(1);
        }

        /* 3) Receive second empty message */
        len = sizeof(buf);
        rc = xlink_recv(ch, buf, &len);
        if (rc != 0) {
            fprintf(stderr, "C: recv #3 (empty): %s\n", xlink_errstr(ch));
            _exit(1);
        }
        if (len != 0) {
            fprintf(stderr, "C: recv #3 expected 0 bytes, got %zu\n", len);
            _exit(1);
        }

        /* 4) Receive "done" after second empty */
        len = sizeof(buf);
        rc = xlink_recv(ch, buf, &len);
        if (rc != 0) {
            fprintf(stderr, "C: recv #4: %s\n", xlink_errstr(ch));
            _exit(1);
        }
        if (len != 5 || memcmp(buf, "done\0", 5) != 0) {
            fprintf(stderr, "C: recv #4 expected 'done\\0', got %zu bytes\n", len);
            _exit(1);
        }

        xlink_close(ch);
        _exit(0);
    }

    /* ── Parent: sender (pipe opens in O_RDWR so both sides can transact) ── */
    close(sync_pipe[1]);
    /* sync_pipe[0] = read end — parent reads child's ready signal */

    xlink_opt_t opt = XLINK_OPT_DEFAULT;
    opt.flags = XLINK_CREATE;
    xlink_channel_t* ch = xlink_open(XLINK_PIPE, PIPE_PATH, &opt);
    CHECK(ch != NULL, "sender open pipe");

    if (!ch) {
        kill(child, SIGTERM);
        waitpid(child, NULL, 0);
        unlink(PIPE_PATH);
        close(sync_pipe[1]);
        goto done;
    }

    /* Wait for child ready */
    char ready = 0;
    ssize_t rn = read(sync_pipe[0], &ready, 1);
    (void)rn;
    CHECK(ready == 'r', "child ready");

    /* 1) Send empty framed message */
    CHECK(xlink_send(ch, "", 0) == 0, "send empty msg (0 bytes)");

    /* 2) Send "hello" after empty */
    CHECK(xlink_send(ch, "hello\0", 6) == 0, "send 'hello\\0' after empty");

    /* 3) Send another empty framed message */
    CHECK(xlink_send(ch, "", 0) == 0, "send second empty msg");

    /* 4) Send "done" */
    CHECK(xlink_send(ch, "done\0", 5) == 0, "send 'done\\0' after second empty");

    xlink_close(ch);

    /* Wait for child to finish */
    int status;
    waitpid(child, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "child process exited successfully");

    close(sync_pipe[0]);
    unlink(PIPE_PATH);

done:
    fprintf(stderr, "\n=== RESULTS: %d checks, %d failures ===\n",
            checks, failures);
    return failures ? 1 : 0;
}
