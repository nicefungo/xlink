/*
 * test_file_nonblock.c — File backend NONBLOCK + raw I/O tests.
 *
 * Covers:
 *   1. File open with XLINK_NONBLOCK flag (accepted, sets O_NONBLOCK)
 *   2. File recv past EOF with NONBLOCK → -1 (EOF, same as blocking)
 *   3. xlink_write() on file backend (.write = NULL, falls back to .send)
 *   4. xlink_read() on file backend (.read = NULL, falls back to .recv)
 *   5. xlink_write on read-only file with NONBLOCK → -1 (EBADF)
 *   6. Mixed: record with xlink_send, replay with xlink_read
 *
 * NOTE: On POSIX, O_NONBLOCK on regular files has no effect on read/write
 * (regular files always appear "ready"). The NONBLOCK flag mainly matters
 * for special files (pipes, sockets, ptys). The file backend accepts it
 * for forward-compatibility with those cases.
 */

#include "xlink.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#define TMP_FILE "/tmp/xlink_test_file_nonblock.bin"

static int failures = 0;
static int checks   = 0;

#define CHECK(cond, msg) do {                                           \
    checks++;                                                           \
    if (!(cond)) {                                                      \
        fprintf(stderr, "  FAIL [%d]: %s\n", checks, msg);             \
        failures++;                                                     \
    } else {                                                            \
        fprintf(stderr, "  PASS [%d]: %s\n", checks, msg);             \
    }                                                                   \
} while(0)

static void test_record_and_replay(void) {
    fprintf(stderr, "\n--- File NONBLOCK: record + replay ---\n");

    unlink(TMP_FILE);

    /* Record with NONBLOCK + CREATE */
    xlink_opt_t opt = XLINK_OPT_DEFAULT;
    opt.flags = XLINK_CREATE | XLINK_NONBLOCK;
    xlink_channel_t* w = xlink_open(XLINK_FILE, TMP_FILE, &opt);
    CHECK(w != NULL, "file open CREATE|NONBLOCK for recording");

    if (!w) { unlink(TMP_FILE); return; }

    const char* msg = "NONBLOCK file record test";
    size_t msglen = strlen(msg) + 1;
    CHECK(xlink_send(w, msg, msglen) == 0,
          "file send via NONBLOCK succeeds");
    xlink_close(w);

    /* Replay with NONBLOCK (no CREATE → read-only) */
    xlink_opt_t ropt = XLINK_OPT_DEFAULT;
    ropt.flags = XLINK_NONBLOCK;
    xlink_channel_t* r = xlink_open(XLINK_FILE, TMP_FILE, &ropt);
    CHECK(r != NULL, "file open NONBLOCK read-only for replay");

    if (r) {
        uint8_t buf[256];
        size_t len = sizeof(buf);
        int rc = xlink_recv(r, buf, &len);
        CHECK(rc == 0, "file recv via NONBLOCK succeeds");
        CHECK(len == msglen, "file recv correct length");
        CHECK(memcmp(buf, msg, len) == 0, "file recv content matches");

        /* Read past EOF: should return -1 with "file: EOF" */
        len = sizeof(buf);
        rc = xlink_recv(r, buf, &len);
        CHECK(rc == -1, "file recv past EOF returns -1 (NONBLOCK)");

        const char* err = xlink_errstr(r);
        CHECK(err != NULL, "file errstr non-NULL after EOF");
        CHECK(strcmp(err, "file: EOF") == 0,
              "file errstr reports 'file: EOF' on NONBLOCK");

        xlink_close(r);
    }

    unlink(TMP_FILE);
}

static void test_xlink_write_raw(void) {
    fprintf(stderr, "\n--- File: xlink_write (NULL .write fallback to .send) ---\n");

    unlink(TMP_FILE);

    /* Create file with NONBLOCK for writing */
    xlink_opt_t opt = XLINK_OPT_DEFAULT;
    opt.flags = XLINK_CREATE | XLINK_NONBLOCK;
    xlink_channel_t* ch = xlink_open(XLINK_FILE, TMP_FILE, &opt);
    CHECK(ch != NULL, "file open CREATE for raw write test");

    if (!ch) { unlink(TMP_FILE); return; }

    /* xlink_write on file backend: .write == NULL, so it calls .send (raw write) */
    const char* raw = "raw-bytes-without-framing";
    size_t rawlen = strlen(raw) + 1;  /* include NUL */
    int rc = xlink_write(ch, raw, rawlen);
    CHECK(rc == 0, "xlink_write raw bytes to file succeeds");
    xlink_close(ch);

    /* Open read-only for verification */
    opt.flags = XLINK_NONBLOCK;
    ch = xlink_open(XLINK_FILE, TMP_FILE, &opt);
    CHECK(ch != NULL, "file open read-only for raw verify");

    if (ch) {
        uint8_t buf[256];
        size_t len = sizeof(buf);

        /* xlink_read on file backend: .read == NULL, so it calls .recv (raw read) */
        rc = xlink_recv(ch, buf, &len);
        CHECK(rc == 0, "xlink_recv raw bytes from file succeeds");
        CHECK(len == rawlen, "xlink_recv raw correct length");
        CHECK(memcmp(buf, raw, len) == 0, "xlink_recv raw content matches");

        /* Read past EOF */
        len = sizeof(buf);
        rc = xlink_recv(ch, buf, &len);
        CHECK(rc == -1, "second recv past EOF returns -1");

        xlink_close(ch);
    }

    unlink(TMP_FILE);
}

static void test_xlink_read_raw(void) {
    fprintf(stderr, "\n--- File: xlink_read (NULL .read fallback to .recv) ---\n");

    unlink(TMP_FILE);

    /* Write to file first using xlink_send */
    xlink_opt_t opt = XLINK_OPT_DEFAULT;
    opt.flags = XLINK_CREATE | XLINK_NONBLOCK;
    xlink_channel_t* w = xlink_open(XLINK_FILE, TMP_FILE, &opt);
    CHECK(w != NULL, "file open CREATE for xlink_read test");

    if (!w) { unlink(TMP_FILE); return; }

    const char* data = "data-for-xlink-read-test";
    size_t datalen = strlen(data) + 1;
    CHECK(xlink_send(w, data, datalen) == 0, "file send data for xlink_read");
    xlink_close(w);

    /* Now read using xlink_read (falls back to .recv since .read = NULL) */
    opt.flags = XLINK_NONBLOCK;
    xlink_channel_t* r = xlink_open(XLINK_FILE, TMP_FILE, &opt);
    CHECK(r != NULL, "file open NONBLOCK for xlink_read test");

    if (r) {
        uint8_t buf[256];
        int n = xlink_read(r, buf, sizeof(buf), -1);
        CHECK(n == (int)datalen, "xlink_read returns full data length");
        if (n > 0) {
            CHECK(memcmp(buf, data, (size_t)n) == 0,
                  "xlink_read content matches");
        }
        xlink_close(r);
    }

    unlink(TMP_FILE);
}

static void test_write_readonly_nonblock(void) {
    fprintf(stderr, "\n--- File: xlink_write to read-only with NONBLOCK ---\n");

    unlink(TMP_FILE);

    /* Write initial content */
    xlink_opt_t opt = XLINK_OPT_DEFAULT;
    opt.flags = XLINK_CREATE;
    xlink_channel_t* w = xlink_open(XLINK_FILE, TMP_FILE, &opt);
    CHECK(w != NULL, "file create for readonly test");
    if (w) {
        CHECK(xlink_send(w, "init", 5) == 0, "write initial content");
        xlink_close(w);
    }

    /* Open read-only with NONBLOCK */
    opt.flags = XLINK_NONBLOCK;
    xlink_channel_t* r = xlink_open(XLINK_FILE, TMP_FILE, &opt);
    CHECK(r != NULL, "file open read-only NONBLOCK");

    if (r) {
        /* xlink_write on read-only should fail (EBADF or EROFS) */
        int rc = xlink_write(r, "data", 5);
        CHECK(rc == -1, "xlink_write to read-only file returns -1");

        const char* err = xlink_errstr(r);
        CHECK(err != NULL, "errstr non-NULL after failed write");
        CHECK(strlen(err) > 0, "errstr non-empty after failed write");
        fprintf(stderr, "    err: %s\n", err);

        /* Read still works */
        uint8_t buf[64];
        size_t len = sizeof(buf);
        rc = xlink_recv(r, buf, &len);
        CHECK(rc == 0, "recv still works on read-only NONBLOCK file");
        CHECK(len == 5 && memcmp(buf, "init", 5) == 0,
              "recv got initial content");

        xlink_close(r);
    }

    unlink(TMP_FILE);
}

int main(void) {
    fprintf(stderr, "=== File backend NONBLOCK + raw I/O tests ===\n");

    test_record_and_replay();
    test_xlink_write_raw();
    test_xlink_read_raw();
    test_write_readonly_nonblock();

    fprintf(stderr, "\n=== RESULTS: %d checks, %d failures ===\n",
            checks, failures);
    return failures ? 1 : 0;
}
