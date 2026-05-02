/*
 * Framing overflow discard test: verify that framed messages exceeding the
 * recv buffer on stream transports (pipe, serial) are discarded without
 * breaking framing sync.
 *
 * Tests the generic frame_recv() discard loop in xlink.c (not the TCP
 * backend's recv_multi). Two scenarios:
 *
 *   Single-chunk discard:
 *     1) Open a pipe
 *     2) Write a large framed message (512 bytes) via raw write
 *     3) Write a small valid framed message ("small_ok")
 *     4) Attempt recv with 64-byte buffer → should report "too large"
 *     5) Recv again with large enough buffer → should get the small message
 *
 *   Multi-chunk discard (>4096 bytes, triggers >1 discard iteration):
 *     1) Write a HUGE framed message (8192 bytes = 2 discard chunks)
 *     2) Write "small_ok"
 *     3) Recv with 64-byte buffer → discards 8192 bytes in 2 chunks
 *     4) Recv with large buffer → gets "small_ok" intact
 *
 * This tests the frame_recv() discard logic added to prevent stream desync,
 * specifically the multi-iteration path where remaining > sizeof(discard).
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
#define HUGE_SZ    8192  /* > 4096 discard chunk, forces multi-pass */

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

static int test_single_chunk(xlink_channel_t* ch, int raw_fd) {
    /* Message 1: large (512 bytes) → too big for 64-byte buffer */
    uint8_t large[LARGE_SZ];
    memset(large, 'A', sizeof(large));
    raw_write_framed(raw_fd, large, sizeof(large));
    CHECK(1, "raw write large framed msg (512 bytes)");

    usleep(50000);

    /* Message 2: small → should survive after discard */
    raw_write_framed(raw_fd, "small_ok", 9);
    CHECK(1, "raw write small framed msg");

    close(raw_fd);

    /* ── Recv with tiny buffer → should get "too large" ── */
    uint8_t buf[TINY_BUF];
    size_t len = sizeof(buf);

    int rc = xlink_recv(ch, buf, &len);
    CHECK(rc != 0, "first recv (oversized) returns error");
    CHECK(len == sizeof(buf), "first recv buffer length unchanged");

    const char* err = xlink_errstr(ch);
    CHECK(err != NULL, "error string is non-NULL");
    printf("  first recv err: %s\n", err);

    /* ── Recv again → should get the small message ── */
    uint8_t buf2[256];
    len = sizeof(buf2);
    rc = xlink_recv(ch, buf2, &len);
    CHECK(rc == 0, "second recv (after discard) succeeds");

    CHECK(len == 9, "small message length match");
    CHECK(memcmp(buf2, "small_ok", 9) == 0, "small message content match");

    return 0;
}

static int test_multi_chunk_discard(xlink_channel_t* ch, int raw_fd) {
    printf("\n--- Multi-chunk discard (>4096 bytes) ---\n");

    /* Message 1: huge (8192 bytes) → triggers 2 discard iterations (chunk=4096) */
    uint8_t huge[HUGE_SZ];
    memset(huge, 'B', sizeof(huge));
    raw_write_framed(raw_fd, huge, sizeof(huge));
    CHECK(1, "raw write huge framed msg (8192 bytes, multi-chunk)");

    usleep(50000);

    /* Message 2: small → should survive after multi-chunk discard */
    raw_write_framed(raw_fd, "small_ok", 9);
    CHECK(1, "raw write small msg after huge");

    close(raw_fd);

    /* ── Recv with tiny buffer → discard 8192 bytes in 2 iterations ── */
    uint8_t buf[TINY_BUF];
    size_t len = sizeof(buf);

    int rc = xlink_recv(ch, buf, &len);
    CHECK(rc != 0, "multi-chunk first recv returns error");
    CHECK(len == sizeof(buf), "multi-chunk buffer length unchanged");

    const char* err = xlink_errstr(ch);
    CHECK(err != NULL, "multi-chunk error string is non-NULL");
    printf("  multi-chunk recv err: %s\n", err);

    /* ── Recv again → should get the small message ── */
    uint8_t buf2[256];
    len = sizeof(buf2);
    rc = xlink_recv(ch, buf2, &len);
    CHECK(rc == 0, "multi-chunk: second recv succeeds after 8KB discard");

    CHECK(len == 9, "multi-chunk: small message length match");
    CHECK(memcmp(buf2, "small_ok", 9) == 0, "multi-chunk: small message content match");

    return 0;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    int ret = 0;

    /* ═══════════════════════════════════════════════════════════
     * Single-chunk discard (512 bytes < 4096 chunk size)
     * ═══════════════════════════════════════════════════════════ */
    unlink(PIPE_PATH);

    printf("=== xlink Framing overflow discard test ===\n");

    xlink_opt_t opt = XLINK_OPT_DEFAULT;
    opt.flags = XLINK_CREATE;

    xlink_channel_t* ch = xlink_open(XLINK_PIPE, PIPE_PATH, &opt);
    CHECK(ch != NULL, "open pipe with CREATE");

    if (!ch) {
        unlink(PIPE_PATH);
        return 1;
    }

    int raw_fd = open(PIPE_PATH, O_WRONLY);
    CHECK(raw_fd >= 0, "raw open pipe for writing");

    if (raw_fd < 0) {
        xlink_close(ch);
        unlink(PIPE_PATH);
        return 1;
    }

    ret |= test_single_chunk(ch, raw_fd);

    xlink_close(ch);
    unlink(PIPE_PATH);

    /* ═══════════════════════════════════════════════════════════
     * Multi-chunk discard (8192 bytes > 4096 chunk size)
     * ═══════════════════════════════════════════════════════════ */
    xlink_opt_t opt2 = XLINK_OPT_DEFAULT;
    opt2.flags = XLINK_CREATE;

    ch = xlink_open(XLINK_PIPE, PIPE_PATH, &opt2);
    CHECK(ch != NULL, "open pipe with CREATE (multi-chunk test)");

    if (!ch) {
        unlink(PIPE_PATH);
        return 1;
    }

    raw_fd = open(PIPE_PATH, O_WRONLY);
    CHECK(raw_fd >= 0, "raw open pipe for writing (multi-chunk test)");

    if (raw_fd >= 0) {
        ret |= test_multi_chunk_discard(ch, raw_fd);
    }

    xlink_close(ch);
    unlink(PIPE_PATH);

    printf("\n=== %s ===\n", failures ? "FAILED" : "ALL PASSED");
    return failures ? 1 : 0;
}
