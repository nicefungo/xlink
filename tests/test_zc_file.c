/*
 * test_zc_file.c — File backend zero-copy send tests.
 *
 * Tests copy_file_range(2) path (fd-to-fd kernel copy) and
 * write() fallback path. Completion via zc_poll().
 */

#include "xlink.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <assert.h>

#define TEST_FILE_SRC  "/tmp/xlink_zc_test_src.dat"
#define TEST_FILE_DST  "/tmp/xlink_zc_test_dst.dat"

static int check_count;

static void pass(const char *msg) {
    printf("  PASS: %s\n", msg);
    check_count++;
}

static void cleanup(void) {
    unlink(TEST_FILE_SRC);
    unlink(TEST_FILE_DST);
}

static int checked_write(int fd, const void *buf, size_t len) {
    ssize_t n = write(fd, buf, len);
    return (n >= 0) ? 0 : -1;
}

int main(void) {
    cleanup();

    printf("=== File Zero-Copy tests ===\n\n");

    /* ── zc_capable ── */
    printf("--- zc_capable ---\n");
    {
        xlink_opt_t opt = { .flags = XLINK_CREATE, .buf_size = 0, .timeout_ms = -1 };
        xlink_channel_t *ch = xlink_open(XLINK_FILE, TEST_FILE_DST, &opt);
        assert(ch);
        pass("open for zc_capable check");

        assert(xlink_zc_capable(ch) == 1);
        pass("file backend zc_capable returns 1");

        xlink_close(ch);
        unlink(TEST_FILE_DST);
    }

    /* ── basic zero-copy send (copy_file_range path) ── */
    printf("--- basic send_zc (copy_file_range) ---\n");
    {
        /* write source data to a real file */
        const char *msg = "hello zero-copy from file!";
        int src_fd = open(TEST_FILE_SRC, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        assert(src_fd >= 0);
        checked_write(src_fd, msg, strlen(msg));
        close(src_fd);

        src_fd = open(TEST_FILE_SRC, O_RDONLY);
        assert(src_fd >= 0);

        xlink_opt_t opt = { .flags = XLINK_CREATE, .buf_size = 0, .timeout_ms = -1 };
        xlink_channel_t *ch = xlink_open(XLINK_FILE, TEST_FILE_DST, &opt);
        assert(ch);
        pass("open target file for zc send");

        xlink_zc_buf_t buf = {
            .addr = (void *)msg,
            .len  = strlen(msg),
            .fd   = src_fd,
            .tag  = 42
        };

        int rc = xlink_send_zc(ch, &buf, NULL, NULL);
        assert(rc == 0);
        pass("send_zc succeeded");

        /* verify completion via zc_poll */
        int n = xlink_zc_poll(ch);
        assert(n == 1);
        pass("zc_poll returns 1 completion");

        xlink_close(ch);
        close(src_fd);

        /* verify destination file has correct data */
        int dst_fd = open(TEST_FILE_DST, O_RDONLY);
        assert(dst_fd >= 0);
        char rbuf[128] = {0};
        ssize_t rn = read(dst_fd, rbuf, sizeof(rbuf) - 1);
        assert(rn > 0);
        rbuf[rn] = '\0';
        assert(strcmp(rbuf, msg) == 0);
        pass("destination file content matches");

        close(dst_fd);
    }
    cleanup();

    /* ── multi-message zero-copy send ── */
    printf("--- multi send_zc ---\n");
    {
        const char *msgs[] = {"one", "two", "three"};
        int src_fd = open(TEST_FILE_SRC, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        assert(src_fd >= 0);
        for (int i = 0; i < 3; i++)
            checked_write(src_fd, msgs[i], strlen(msgs[i]));
        close(src_fd);

        src_fd = open(TEST_FILE_SRC, O_RDONLY);
        assert(src_fd >= 0);

        xlink_opt_t opt = { .flags = XLINK_CREATE, .buf_size = 0, .timeout_ms = -1 };
        xlink_channel_t *ch = xlink_open(XLINK_FILE, TEST_FILE_DST, &opt);
        assert(ch);

        for (int i = 0; i < 3; i++) {
            xlink_zc_buf_t buf = {
                .addr = (void *)msgs[i],
                .len  = strlen(msgs[i]),
                .fd   = src_fd,
                .tag  = (uint64_t)(100 + i)
            };
            int rc = xlink_send_zc(ch, &buf, NULL, NULL);
            assert(rc == 0);
        }
        pass("3x send_zc succeeded");

        int n = xlink_zc_poll(ch);
        assert(n == 3);
        pass("zc_poll returns 3 completions");

        xlink_close(ch);
        close(src_fd);

        /* verify all 3 msgs concatenated */
        int dst_fd = open(TEST_FILE_DST, O_RDONLY);
        assert(dst_fd >= 0);
        char rbuf[256] = {0};
        ssize_t rn = read(dst_fd, rbuf, sizeof(rbuf) - 1);
        assert(rn == 11); /* "one"+"two"+"three" = 11 */
        rbuf[rn] = '\0';
        pass("multi content length == 11");
        assert(strncmp(rbuf, "onetwothree", 11) == 0);
        pass("multi content match");

        close(dst_fd);
    }
    cleanup();

    /* ── send_zc with NULL fd (write() fallback) ── */
    printf("--- send_zc fallback (no src fd) ---\n");
    {
        const char *msg = "fallback write path";
        xlink_opt_t opt = { .flags = XLINK_CREATE, .buf_size = 0, .timeout_ms = -1 };
        xlink_channel_t *ch = xlink_open(XLINK_FILE, TEST_FILE_DST, &opt);
        assert(ch);

        xlink_zc_buf_t buf = {
            .addr = (void *)msg,
            .len  = strlen(msg),
            .fd   = -1,   /* triggers fallback */
            .tag  = 1
        };

        int rc = xlink_send_zc(ch, &buf, NULL, NULL);
        assert(rc == 0);
        pass("fallback send_zc succeeded");

        int n = xlink_zc_poll(ch);
        assert(n == 1);
        pass("fallback zc_poll");

        xlink_close(ch);

        /* verify */
        int dst_fd = open(TEST_FILE_DST, O_RDONLY);
        assert(dst_fd >= 0);
        char rbuf[128] = {0};
        ssize_t rn = read(dst_fd, rbuf, sizeof(rbuf) - 1);
        rbuf[rn] = '\0';
        assert(strcmp(rbuf, msg) == 0);
        pass("fallback content matches");

        close(dst_fd);
    }
    cleanup();

    /* ── error paths ── */
    printf("--- error paths ---\n");
    {
        xlink_opt_t opt = { .flags = XLINK_CREATE, .buf_size = 0, .timeout_ms = -1 };
        xlink_channel_t *ch = xlink_open(XLINK_FILE, TEST_FILE_DST, &opt);
        assert(ch);

        /* NULL buf */
        int rc = xlink_send_zc(ch, NULL, NULL, NULL);
        assert(rc == -1);
        pass("send_zc NULL returns -1");

        /* empty buffer */
        xlink_zc_buf_t empty = { .addr = (void *)"x", .len = 0, .fd = -1, .tag = 1 };
        rc = xlink_send_zc(ch, &empty, NULL, NULL);
        assert(rc == -1);
        pass("send_zc empty buf returns -1");

        xlink_close(ch);
    }
    cleanup();

    printf("\n=== RESULTS: %d checks, 0 failures ===\n", check_count);
    return 0;
}
