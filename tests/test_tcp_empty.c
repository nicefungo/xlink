/*
 * test_tcp_empty.c — Test TCP client-mode framing with 0-length messages.
 *
 * TCP client mode uses write_framed/read_framed internally (not the generic
 * frame_send/frame_recv from xlink.c). This test verifies that:
 *
 *   1) Sending an empty (0-length) framed message over TCP succeeds
 *   2) Server receives the empty message correctly (*len = 0)
 *   3) Normal messages still work after an empty one (framing stays in sync)
 *   4) Multiple empty messages in sequence don't corrupt framing
 */

#include "xlink.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>

#define PORT     ":19994"
#define ADDR     "127.0.0.1:19994"
#define BUFSZ    256

static int failures = 0;
#define CHECK(cond, msg) do {                                           \
    if (!(cond)) {                                                      \
        fprintf(stderr, "  FAIL: %s\n", msg);                          \
        failures++;                                                     \
    } else {                                                            \
        fprintf(stderr, "  PASS: %s\n", msg);                          \
    }                                                                   \
} while(0)

int main(void) {
    signal(SIGPIPE, SIG_IGN);

    int child_pipe[2];
    if (pipe(child_pipe) != 0) { perror("pipe"); return 1; }

    pid_t svr_pid = fork();
    if (svr_pid == 0) {
        /* ── Server process ── */
        close(child_pipe[0]);

        xlink_opt_t opt = XLINK_OPT_DEFAULT;
        opt.flags = XLINK_SERVER;
        xlink_channel_t* ch = xlink_open(XLINK_TCP, PORT, &opt);
        if (!ch) { fprintf(stderr, "S: open fail\n"); _exit(1); }

        /* ── Receive messages in order ── */

        /* 1) Empty message */
        uint8_t buf[BUFSZ];
        size_t len = sizeof(buf);
        if (xlink_recv(ch, buf, &len) != 0) {
            fprintf(stderr, "S: recv #1 (empty): %s\n", xlink_errstr(ch));
            _exit(1);
        }
        if (len != 0) {
            fprintf(stderr, "S: recv #1 expected 0 bytes, got %zu\n", len);
            _exit(1);
        }

        /* 2) Normal message after empty */
        len = sizeof(buf);
        if (xlink_recv(ch, buf, &len) != 0) {
            fprintf(stderr, "S: recv #2 (normal): %s\n", xlink_errstr(ch));
            _exit(1);
        }
        if (len != 5 || memcmp(buf, "hello", 5) != 0) {
            fprintf(stderr, "S: recv #2 expected 'hello', got %zu bytes\n", len);
            _exit(1);
        }

        /* 3) Second empty message */
        len = sizeof(buf);
        if (xlink_recv(ch, buf, &len) != 0) {
            fprintf(stderr, "S: recv #3 (empty): %s\n", xlink_errstr(ch));
            _exit(1);
        }
        if (len != 0) {
            fprintf(stderr, "S: recv #3 expected 0 bytes, got %zu\n", len);
            _exit(1);
        }

        /* 4) Normal message after second empty */
        len = sizeof(buf);
        if (xlink_recv(ch, buf, &len) != 0) {
            fprintf(stderr, "S: recv #4 (normal): %s\n", xlink_errstr(ch));
            _exit(1);
        }
        if (len != 4 || memcmp(buf, "done", 4) != 0) {
            fprintf(stderr, "S: recv #4 expected 'done', got %zu bytes\n", len);
            _exit(1);
        }

        xlink_close(ch);
        ssize_t w = write(child_pipe[1], "ok", 3);
        (void)w;
        _exit(0);
    }

    /* ── Parent: TCP client ── */
    close(child_pipe[1]);

    xlink_channel_t* ch = NULL;
    for (int retry = 0; retry < 10; retry++) {
        ch = xlink_open(XLINK_TCP, ADDR, NULL);
        if (ch) break;
        usleep(150000);
    }
    CHECK(ch != NULL, "client open");

    if (!ch) {
        kill(svr_pid, SIGTERM);
        waitpid(svr_pid, NULL, 0);
        close(child_pipe[0]);
        goto done;
    }

    /* 1) Send empty message (0-length payload) */
    CHECK(xlink_send(ch, "", 0) == 0, "send empty msg");

    /* 2) Send normal message */
    CHECK(xlink_send(ch, "hello", 5) == 0, "send 'hello' after empty");

    /* 3) Send another empty message */
    CHECK(xlink_send(ch, "", 0) == 0, "send second empty msg");

    /* 4) Send final normal message */
    CHECK(xlink_send(ch, "done", 4) == 0, "send 'done' after second empty");

    xlink_close(ch);

    /* Wait for server */
    char result[16] = {0};
    ssize_t n = read(child_pipe[0], result, sizeof(result) - 1);
    if (n > 0) result[n] = '\0';
    CHECK(strcmp(result, "ok") == 0, "server received all messages in order");
    close(child_pipe[0]);

    waitpid(svr_pid, NULL, 0);

done:
    fprintf(stderr, "=== %s ===\n", failures ? "FAILED" : "ALL PASSED");
    return failures ? 1 : 0;
}
