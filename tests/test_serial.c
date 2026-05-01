/*
 * Serial backend test via pseudo-terminal.
 *
 * Uses posix_openpt() to create a PTY pair:
 *   slave → opened as serial channel
 *   master → used to send/receive test data
 */

#define _GNU_SOURCE
#include "xlink.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#if defined(__linux__)
#  include <pty.h>
#else
#  include <util.h>
#endif

static int test_master_to_serial(void) {
    int master_fd = posix_openpt(O_RDWR | O_NOCTTY);
    if (master_fd < 0) {
        perror("posix_openpt");
        return -1;
    }

    if (grantpt(master_fd) != 0 || unlockpt(master_fd) != 0) {
        perror("grantpt/unlockpt");
        close(master_fd);
        return -1;
    }

    const char* slave_name = ptsname(master_fd);
    if (!slave_name) {
        perror("ptsname");
        close(master_fd);
        return -1;
    }

    printf("  PTY: master=%d, slave=%s\n", master_fd, slave_name);

    /* ── Open serial channel on slave ── */
    xlink_opt_t opt = XLINK_OPT_DEFAULT;
    opt.flags = XLINK_CREATE;

    xlink_channel_t* ch = xlink_open(XLINK_SERIAL, slave_name, &opt);
    if (!ch) {
        fprintf(stderr, "FAIL: serial open(%s)\n", slave_name);
        close(master_fd);
        return 1;
    }

    /* ── Send via master PTY (must include framing header) ── */
    const char* msg = "Hello serial!";
    size_t      msglen = strlen(msg) + 1;

    uint8_t frame[4 + 256];
    frame[0] = (uint8_t)(msglen >> 24);
    frame[1] = (uint8_t)(msglen >> 16);
    frame[2] = (uint8_t)(msglen >>  8);
    frame[3] = (uint8_t)(msglen >>  0);
    memcpy(frame + 4, msg, msglen);

    if (write(master_fd, frame, 4 + msglen) < 0) {
        fprintf(stderr, "FAIL: master write\n");
        close(master_fd);
        xlink_close(ch);
        return 1;
    }

    uint8_t buf[4096];
    size_t  len = sizeof(buf);
    if (xlink_recv(ch, buf, &len) != 0) {
        fprintf(stderr, "FAIL: serial recv: %s\n", xlink_errstr(ch));
        close(master_fd);
        xlink_close(ch);
        return 1;
    }

    if (len != msglen || memcmp(buf, msg, msglen) != 0) {
        fprintf(stderr, "FAIL: data mismatch (%zu vs %zu)\n", len, msglen);
        close(master_fd);
        xlink_close(ch);
        return 1;
    }

    printf("  Serial master→slave: %zu bytes OK\n", len);

    /* ── Send via serial, recv via master ── */
    const char* reply = "Reply via serial!";
    size_t      replylen = strlen(reply) + 1;

    if (xlink_send(ch, reply, replylen) != 0) {
        fprintf(stderr, "FAIL: serial send: %s\n", xlink_errstr(ch));
        close(master_fd);
        xlink_close(ch);
        return 1;
    }

    len = sizeof(buf);
    ssize_t n = read(master_fd, buf, len);
    if (n < 0) {
        fprintf(stderr, "FAIL: master read\n");
        close(master_fd);
        xlink_close(ch);
        return 1;
    }
    len = (size_t)n;

    /* Serial uses framing (4-byte prefix) */
    uint8_t expected[4 + 256];
    expected[0] = (uint8_t)(replylen >> 24);
    expected[1] = (uint8_t)(replylen >> 16);
    expected[2] = (uint8_t)(replylen >>  8);
    expected[3] = (uint8_t)(replylen >>  0);
    memcpy(expected + 4, reply, replylen);

    size_t expected_total = 4 + replylen;

    if (len != expected_total || memcmp(buf, expected, expected_total) != 0) {
        fprintf(stderr, "FAIL: master got %zu bytes, expected %zu (framed)\n",
                len, expected_total);
        close(master_fd);
        xlink_close(ch);
        return 1;
    }

    printf("  Serial slave→master: %zu bytes framed OK\n", replylen);

    xlink_close(ch);
    close(master_fd);
    return 0;
}

int main(void) {
    int failures = 0;
    printf("=== xlink Serial test ===\n");
    failures += test_master_to_serial();
    printf("=== %s ===\n", failures ? "FAILED" : "ALL PASSED");
    return failures ? 1 : 0;
}
