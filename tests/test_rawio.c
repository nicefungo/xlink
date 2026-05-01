/*
 * Raw I/O test — xlink_write() and xlink_read() bypass framing.
 *
 * These low-level APIs are the only path that exercises the backend's
 * send/recv directly (not through xlink.c frame_send/frame_recv).
 *
 * Strategy: for FIFO opened O_RDWR, written data is immediately
 * readable from the same fd (self-loop). xlink_read still uses the
 * backend's recv internally, so we sequence writes before reads.
 *
 * Tests:
 *   1. Pipe: xlink_write → xlink_read raw bytes (single channel)
 *   2. Pipe: mix xlink_send + xlink_write on separate channels
 *   3. Pipe: xlink_write empty via xlink_send (ensure send path)
 */

#include "xlink.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

static int failures = 0;
#define CHECK(cond, msg) do {                                             \
    if (!(cond)) {                                                        \
        fprintf(stderr, "  FAIL [%d]: %s\n", __LINE__, msg);              \
        failures++;                                                       \
    } else {                                                              \
        printf("  PASS: %s\n", msg);                                      \
    }                                                                     \
} while(0)

static const char* FIFO = "/tmp/xlink_test_rawio";

static int test_single_channel_raw(void) {
    printf("\n--- Pipe: xlink_write / xlink_read single channel ---\n");

    unlink(FIFO);

    xlink_opt_t opt = XLINK_OPT_DEFAULT;
    opt.flags = XLINK_CREATE;

    xlink_channel_t* ch = xlink_open(XLINK_PIPE, FIFO, &opt);
    CHECK(ch != NULL, "open pipe with CREATE");
    if (!ch) { unlink(FIFO); return 1; }

    /* Write raw bytes (no framing header) via xlink_write */
    const char* raw = "raw-data-without-framing";
    size_t rawlen = strlen(raw);
    int rc = xlink_write(ch, raw, rawlen);
    CHECK(rc == 0, "xlink_write raw bytes");

    /* Read raw bytes back via xlink_read (blocking, data already written) */
    uint8_t buf[256];
    int n = xlink_read(ch, buf, sizeof(buf), -1);
    CHECK(n == (int)rawlen, "xlink_read returns exact byte count");
    if (n == (int)rawlen) {
        CHECK(memcmp(buf, raw, (size_t)n) == 0, "xlink_read content match");
    }

    xlink_close(ch);
    unlink(FIFO);
    return 0;
}

static int test_pipe_mixed_usage(void) {
    printf("\n--- Pipe: mix of xlink_send (framed) and xlink_write (raw) ---\n");

    unlink(FIFO);

    xlink_opt_t opt = XLINK_OPT_DEFAULT;
    opt.flags = XLINK_CREATE;

    /* Two separate channels sharing the same FIFO */
    xlink_channel_t* w = xlink_open(XLINK_PIPE, FIFO, &opt);
    CHECK(w != NULL, "open writer pipe");
    if (!w) { unlink(FIFO); return 1; }

    xlink_channel_t* r = xlink_open(XLINK_PIPE, FIFO, NULL);
    CHECK(r != NULL, "open reader pipe");
    if (!r) { xlink_close(w); unlink(FIFO); return 1; }

    /* Send framed message via xlink_send (writes 4-byte header + payload) */
    const char* framed = "framed-hello";
    int rc = xlink_send(w, framed, strlen(framed) + 1);
    CHECK(rc == 0, "xlink_send framed message");

    /* Receive framed via xlink_recv */
    uint8_t buf[256];
    size_t len = sizeof(buf);
    rc = xlink_recv(r, buf, &len);
    CHECK(rc == 0 && len == strlen(framed) + 1 &&
          memcmp(buf, framed, len) == 0, "xlink_recv matched framed msg");

    /* Write raw bytes via xlink_write (no framing header) */
    const char* raw = "|raw-bytes|";
    rc = xlink_write(w, raw, strlen(raw));
    CHECK(rc == 0, "xlink_write raw after framed send");

    /* Read raw via xlink_read (blocking, data already written) */
    int n = xlink_read(r, buf, sizeof(buf), -1);
    CHECK(n == (int)strlen(raw) &&
          memcmp(buf, raw, (size_t)n) == 0, "xlink_read raw content");

    xlink_close(w);
    xlink_close(r);
    unlink(FIFO);
    return 0;
}

static int test_pipe_send_empty(void) {
    printf("\n--- Pipe: xlink_send empty (framed, goes through frame_send path) ---\n");

    unlink(FIFO);

    xlink_opt_t opt = XLINK_OPT_DEFAULT;
    opt.flags = XLINK_CREATE;

    xlink_channel_t* ch = xlink_open(XLINK_PIPE, FIFO, &opt);
    CHECK(ch != NULL, "open pipe");
    if (!ch) { unlink(FIFO); return 1; }

    /* xlink_send with 0 bytes — frame_send writes 4-byte header with len=0 */
    int rc = xlink_send(ch, "", 0);
    CHECK(rc == 0, "xlink_send empty (0 bytes) succeeds");

    uint8_t buf[256];
    size_t len = sizeof(buf);
    rc = xlink_recv(ch, buf, &len);
    CHECK(rc == 0 && len == 0, "xlink_recv gets 0-byte empty framed msg");

    xlink_close(ch);
    unlink(FIFO);
    return 0;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);

    printf("=== xlink raw I/O tests ===\n");
    test_single_channel_raw();
    test_pipe_mixed_usage();
    test_pipe_send_empty();
    printf("\n=== %s ===\n", failures ? "FAILED" : "ALL PASSED");
    return failures ? 1 : 0;
}
