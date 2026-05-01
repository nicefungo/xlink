/*
 * File backend test — record then replay in same process.
 */

#include "xlink.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char* TMP_FILE = "/tmp/xlink_test_file.bin";

static int failures = 0;
#define CHECK(cond, msg) do {                                   \
    if (!(cond)) {                                              \
        fprintf(stderr, "  FAIL: %s\n", msg);                   \
        failures++;                                             \
    } else {                                                    \
        printf("  PASS: %s\n", msg);                            \
    }                                                           \
} while(0)

static int test_record_replay(void) {
    const char* msg = "Hello via file!";
    size_t      msglen = strlen(msg) + 1;

    /* ── RECORD: create file, write message ── */
    xlink_opt_t opt = XLINK_OPT_DEFAULT;
    opt.flags = XLINK_CREATE;

    xlink_channel_t* w = xlink_open(XLINK_FILE, TMP_FILE, &opt);
    if (!w) {
        fprintf(stderr, "FAIL: writer open\n");
        return 1;
    }

    if (xlink_send(w, msg, msglen) != 0) {
        fprintf(stderr, "FAIL: writer send: %s\n", xlink_errstr(w));
        xlink_close(w);
        return 1;
    }
    xlink_close(w);

    /* ── REPLAY: open for reading, read message ── */
    xlink_opt_t opt_r = XLINK_OPT_DEFAULT;
    /* no CREATE → read-only */

    xlink_channel_t* r = xlink_open(XLINK_FILE, TMP_FILE, &opt_r);
    if (!r) {
        fprintf(stderr, "FAIL: reader open\n");
        return 1;
    }

    uint8_t buf[4096];
    size_t  len = sizeof(buf);
    if (xlink_recv(r, buf, &len) != 0) {
        fprintf(stderr, "FAIL: reader recv: %s\n", xlink_errstr(r));
        xlink_close(r);
        return 1;
    }

    if (len != msglen || memcmp(buf, msg, msglen) != 0) {
        fprintf(stderr, "FAIL: data mismatch (%zu vs %zu)\n", len, msglen);
        xlink_close(r);
        return 1;
    }

    xlink_close(r);
    unlink(TMP_FILE);

    printf("  File record+replay: %zu bytes OK\n", len);
    return 0;
}

static void test_file_eof(void) {
    printf("\n--- File backend: read past EOF ---\n");

    /* Write 1 byte, then read it and try to read past EOF */
    xlink_opt_t opt = XLINK_OPT_DEFAULT;
    opt.flags = XLINK_CREATE;

    xlink_channel_t* w = xlink_open(XLINK_FILE, TMP_FILE, &opt);
    CHECK(w != NULL, "file create for EOF test");
    if (w) {
        uint8_t data = 42;
        CHECK(xlink_send(w, &data, 1) == 0, "file write 1 byte");
        xlink_close(w);
    }

    /* Open read-only */
    xlink_channel_t* r = xlink_open(XLINK_FILE, TMP_FILE, NULL);
    CHECK(r != NULL, "file open (read-only) for EOF test");

    if (r) {
        uint8_t buf[64];
        size_t len = sizeof(buf);

        /* First read should succeed */
        int rc = xlink_recv(r, buf, &len);
        CHECK(rc == 0, "file recv succeeds (1 byte available)");
        CHECK(len == 1, "file recv got 1 byte");
        CHECK(buf[0] == 42, "file recv content match");

        /* Second read: past EOF */
        len = sizeof(buf);
        rc = xlink_recv(r, buf, &len);
        CHECK(rc == -1, "file recv past EOF returns -1");

        const char* err = xlink_errstr(r);
        CHECK(err != NULL && strcmp(err, "file: EOF") == 0,
              "file recv EOF error string is 'file: EOF'");
        printf("    err: %s\n", err);

        /* Third read: still past EOF */
        len = sizeof(buf);
        rc = xlink_recv(r, buf, &len);
        CHECK(rc == -1, "file recv after EOF still returns -1");

        xlink_close(r);
    }
    unlink(TMP_FILE);
}

int main(void) {
    printf("=== xlink File test ===\n");
    failures += test_record_replay();
    test_file_eof();
    printf("=== %s ===\n", failures ? "FAILED" : "ALL PASSED");
    return failures ? 1 : 0;
}
