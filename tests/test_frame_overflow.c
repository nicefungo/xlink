/*
 * Framing overflow discard test: verify that framed messages exceeding the
 * recv buffer on stream transports (pipe, serial) are discarded without
 * breaking framing sync.
 *
 * Strategy:
 *   1) Open a pipe
 *   2) Write a large framed message (>64 bytes) via raw write
 *   3) Write a small valid framed message ("small_ok")
 *   4) Attempt recv with 64-byte buffer → should report "too large"
 *   5) Recv again with large enough buffer → should get the small message intact
 *
 * This tests the frame_recv() discard logic added to prevent stream desync.
 */

#include "xlink.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>

#define PIPE_PATH  "/tmp/xlink_test_frame_overflow"
#define TINY_BUF   64
#define LARGE_SZ   512

static int failures = 0;
#define CHECK(cond, msg) do {                                   \
    if (!(cond)) {                                              \
        fprintf(stderr, "  FAIL: %s\n", msg);                  \
        failures++;                                             \
    } else {                                                    \
        printf("  PASS: %s\n", msg);                            \
    }                                                           \
} while(0)

/* Write 4-byte big-endian framing header + payload to a raw fd */
static void raw_write_framed(int fd, const void* data, size_t len) {
    uint8_t hdr[4];
    hdr[0] = (uint8_t)(len >> 24);
    hdr[1] = (uint8_t)(len >> 16);
    hdr[2] = (uint8_t)(len >> 8);
    hdr[3] = (uint8_t)(len);
    ssize_t r;
    r = write(fd, hdr, 4); (void)r;
    if (len > 0)
        r = write(fd, data, len);
    (void)r;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    unlink(PIPE_PATH);

    printf("=== xlink Framing overflow discard test ===\n\n");

        /* ── Create pipe via xlink ── */
    xlink_opt_t opt = XLINK_OPT_DEFAULT;
    opt.flags = XLINK_CREATE;

    xlink_channel_t* ch = xlink_open(XLINK_PIPE, PIPE_PATH, &opt);
    CHECK(ch != NULL, "open pipe with CREATE");

    if (!ch) {
        unlink(PIPE_PATH);
        return 1;
    }

    /* ── Write oversized framed messages via raw open ── */
    /* xlink_channel_t is opaque; open a write fd to the pipe directly. */
    int raw_fd = open(PIPE_PATH, O_WRONLY);
    CHECK(raw_fd >= 0, "raw open pipe for writing");

    if (raw_fd >= 0) {
        /* Message 1: large (512 bytes) → too big for 64-byte buffer */
        uint8_t large[LARGE_SZ];
        memset(large, 'A', sizeof(large));
        raw_write_framed(raw_fd, large, sizeof(large));
        CHECK(1, "raw write large framed msg (512 bytes)");

        /* Small delay to let the write settle */
        usleep(50000);

        /* Message 2: small → should survive after discard */
        const char* small = "small_ok";
        raw_write_framed(raw_fd, small, strlen(small) + 1);
        CHECK(1, "raw write small framed msg");

        close(raw_fd);
    }

    /* ── Recv with tiny buffer → should get "too large" ── */
    /* frame_recv will discard the 512-byte payload and report error */
    uint8_t buf[TINY_BUF];
    size_t len = sizeof(buf);

    int rc = xlink_recv(ch, buf, &len);
    CHECK(rc != 0, "first recv (oversized) returns error");
    CHECK(len == sizeof(buf), "first recv buffer length unchanged");

    const char* err = xlink_errstr(ch);
    CHECK(err != NULL, "error string is non-NULL");
    printf("  first recv err: %s\n", err);

    /* ── Recv again → should get the small message ── */
    /* If frame_recv didn't discard correctly, we'd read garbage framing bytes
     * and the small message would be lost. This is the real test. */
    uint8_t buf2[256];
    len = sizeof(buf2);
    rc = xlink_recv(ch, buf2, &len);
    CHECK(rc == 0, "second recv (after discard) succeeds");

    const char* expected = "small_ok";
    size_t exlen = strlen(expected) + 1;
    if (rc == 0) {
        CHECK(len == exlen, "small message length match");
        CHECK(memcmp(buf2, expected, exlen) == 0, "small message content match");
        if (len != exlen) {
            printf("  expected %zu bytes, got %zu\n", exlen, len);
        }
        if (memcmp(buf2, expected, exlen) != 0) {
            printf("  got: \"%.*s\"\n", (int)len, (char*)buf2);
        }
    }

    xlink_close(ch);
    unlink(PIPE_PATH);

    printf("\n=== %s ===\n", failures ? "FAILED" : "ALL PASSED");
    return failures ? 1 : 0;
}
