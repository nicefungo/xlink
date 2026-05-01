/*
 * Pipe (FIFO) round-trip test via xlink API.
 *
 * Creates a FIFO, sends a message, receives it, verifies.
 * Pipe is unidirectional, but we open O_RDWR so same channel
 * can both send and recv via framing layer.
 */

#include "xlink.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

static const char* FIFO_PATH = "/tmp/xlink_test_pipe";

static int test_roundtrip(void) {
    /* Remove stale FIFO */
    unlink(FIFO_PATH);

    /* ── Open channel (with O_RDWR, framing auto-enabled) ── */
    xlink_opt_t opt = XLINK_OPT_DEFAULT;
    opt.flags = XLINK_CREATE;

    xlink_channel_t* ch = xlink_open(XLINK_PIPE, FIFO_PATH, &opt);
    if (!ch) {
        fprintf(stderr, "FAIL: open pipe\n");
        return 1;
    }

    const char* msg = "Hello via pipe!";
    size_t      msglen = strlen(msg) + 1;

    if (xlink_send(ch, msg, msglen) != 0) {
        fprintf(stderr, "FAIL: send: %s\n", xlink_errstr(ch));
        xlink_close(ch);
        return 1;
    }

    /* Receive back (same fd — O_RDWR) */
    uint8_t buf[4096];
    size_t  len = sizeof(buf);
    if (xlink_recv(ch, buf, &len) != 0) {
        fprintf(stderr, "FAIL: recv: %s\n", xlink_errstr(ch));
        xlink_close(ch);
        return 1;
    }

    if (len != msglen || memcmp(buf, msg, msglen) != 0) {
        fprintf(stderr, "FAIL: data mismatch (got %zu bytes, expected %zu)\n",
                len, msglen);
        xlink_close(ch);
        return 1;
    }

    xlink_close(ch);
    unlink(FIFO_PATH);

    printf("  Pipe round-trip: %zu bytes OK\n", len);
    return 0;
}

/*
 * Test two-process style:
 *   create writer → send
 *   open reader  → receive
 *   (same process, sequential — O_RDWR handle allows both)
 */
static int test_writer_reader(void) {
    unlink(FIFO_PATH);

    /* Writer */
    xlink_opt_t opt_w = XLINK_OPT_DEFAULT;
    opt_w.flags = XLINK_CREATE;

    xlink_channel_t* tx = xlink_open(XLINK_PIPE, FIFO_PATH, &opt_w);
    if (!tx) {
        fprintf(stderr, "FAIL: writer open\n");
        return 1;
    }

    const char* msg = "multi-channel pipe test";
    size_t      msglen = strlen(msg) + 1;

    if (xlink_send(tx, msg, msglen) != 0) {
        fprintf(stderr, "FAIL: writer send: %s\n", xlink_errstr(tx));
        xlink_close(tx);
        return 1;
    }

    /* Reader */
    xlink_opt_t opt_r = XLINK_OPT_DEFAULT;
    /* no CREATE */

    xlink_channel_t* rx = xlink_open(XLINK_PIPE, FIFO_PATH, &opt_r);
    if (!rx) {
        fprintf(stderr, "FAIL: reader open\n");
        xlink_close(tx);
        return 1;
    }

    uint8_t buf[4096];
    size_t  len = sizeof(buf);
    if (xlink_recv(rx, buf, &len) != 0) {
        fprintf(stderr, "FAIL: reader recv: %s\n", xlink_errstr(rx));
        xlink_close(tx);
        xlink_close(rx);
        return 1;
    }

    if (len != msglen || memcmp(buf, msg, msglen) != 0) {
        fprintf(stderr, "FAIL: data mismatch (%zu vs %zu)\n", len, msglen);
        xlink_close(tx);
        xlink_close(rx);
        return 1;
    }

    xlink_close(tx);
    xlink_close(rx);
    unlink(FIFO_PATH);

    printf("  Pipe writer+reader: %zu bytes OK\n", len);
    return 0;
}

int main(void) {
    int failures = 0;
    printf("=== xlink Pipe test ===\n");
    failures += test_roundtrip();
    failures += test_writer_reader();
    printf("=== %s ===\n", failures ? "FAILED" : "ALL PASSED");
    return failures ? 1 : 0;
}
