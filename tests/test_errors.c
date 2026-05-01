/*
 * Error handling tests — invalid inputs, missing resources, edge cases.
 */

#include "xlink.h"
#include "shm_ipc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>

static int failures = 0;
#define CHECK(cond, msg) do {                                   \
    if (!(cond)) {                                              \
        fprintf(stderr, "  FAIL: %s\n", msg);                   \
        failures++;                                             \
    } else {                                                    \
        printf("  PASS: %s\n", msg);                            \
    }                                                           \
} while(0)
#define CHECK_STR(actual, expected) do {                         \
    if (strcmp(actual, expected) != 0) {                         \
        fprintf(stderr, "  FAIL: expected '%s', got '%s'\n",    \
                expected, actual);                               \
        failures++;                                             \
    } else {                                                    \
        printf("  PASS: '%s' == '%s'\n", expected, actual);     \
    }                                                           \
} while(0)

static void test_null_channel(void) {
    printf("\n--- NULL / invalid handle ---\n");

    /* xlink_close(NULL) must not crash */
    xlink_close(NULL);    /* should be safe */
    CHECK(1, "xlink_close(NULL)");

    /* xlink_errstr(NULL) must not crash */
    const char* s = xlink_errstr(NULL);
    CHECK(s != NULL, "xlink_errstr(NULL) returns string");
}

static void test_invalid_open(void) {
    printf("\n--- Invalid open ---\n");

    /* Unknown type */
    xlink_channel_t* ch = xlink_open((xlink_type_t)999, "/dev/null", NULL);
    CHECK(ch == NULL, "xlink_open(invalid type) returns NULL");

    /* Non-existent pipe without CREATE */
    ch = xlink_open(XLINK_PIPE, "/tmp/xlink_nonexistent_fifo_test", NULL);
    CHECK(ch == NULL, "xlink_open(pipe, nonexistent) returns NULL");

    /* Non-existent file for reading */
    ch = xlink_open(XLINK_FILE, "/tmp/xlink_nonexistent_file_test", NULL);
    CHECK(ch == NULL, "xlink_open(file, nonexistent) returns NULL");

    /* Malformed TCP address */
    ch = xlink_open(XLINK_TCP, "bad-addr", NULL);
    CHECK(ch == NULL, "xlink_open(tcp, bad-addr) returns NULL");

    /* Malformed UDP address (no port) */
    ch = xlink_open(XLINK_UDP, "localhost", NULL);
    CHECK(ch == NULL, "xlink_open(udp, no-port) returns NULL");
}

static void test_empty_message(void) {
    printf("\n--- Empty / zero-length message ---\n");

    /* Pipe: framing should handle zero-length message */
    unlink("/tmp/xlink_test_empty");

    xlink_opt_t opt = XLINK_OPT_DEFAULT;
    opt.flags = XLINK_CREATE;

    xlink_channel_t* ch = xlink_open(XLINK_PIPE, "/tmp/xlink_test_empty", &opt);
    CHECK(ch != NULL, "pipe create");

    if (ch) {
        int rc = xlink_send(ch, "", 0);
        CHECK(rc == 0, "pipe send(empty, 0) succeeds");

        uint8_t buf[16];
        size_t len = sizeof(buf);
        rc = xlink_recv(ch, buf, &len);
        CHECK(rc == 0 && len == 0, "pipe recv gets 0 bytes");

        xlink_close(ch);
    }
    unlink("/tmp/xlink_test_empty");
}

static void test_file_write_readonly(void) {
    printf("\n--- File backend: write to read-only fd ---\n");

    const char* tmp = "/tmp/xlink_test_err_file";
    unlink(tmp);

    /* Create a file for writing first */
    xlink_opt_t opt_w = XLINK_OPT_DEFAULT;
    opt_w.flags = XLINK_CREATE;
    xlink_channel_t* w = xlink_open(XLINK_FILE, tmp, &opt_w);
    CHECK(w != NULL, "file open (CREATE) for write test");

    if (w) {
        /* Write some initial data */
        const char* init = "hello";
        int rc = xlink_send(w, init, strlen(init) + 1);
        CHECK(rc == 0, "initial file write succeeds");
        xlink_close(w);
    }

    /* Now open the same file WITHOUT CREATE (O_RDONLY) and try to write */
    xlink_channel_t* r = xlink_open(XLINK_FILE, tmp, NULL);
    CHECK(r != NULL, "file open (read-only) succeeds");

    if (r) {
        const char* data = "should fail";
        int rc = xlink_send(r, data, strlen(data) + 1);
        CHECK(rc == -1, "file send on read-only fd returns -1");

        /* errbuf should contain an error message */
        const char* err = xlink_errstr(r);
        CHECK(err != NULL && strlen(err) > 0, "xlink_errstr returns non-empty error");
        printf("    err: %s\n", err);

        xlink_close(r);
    }

    unlink(tmp);
}

static void test_rtsp_not_implemented(void) {
    printf("\n--- RTSP type (no backend) ---\n");

    /* XLINK_RTSP is a valid enum member but has no backend registered.
     * xlink_open should return NULL with errno = ENOSYS. */
    const char* rtsp_url = "rtsp://localhost/test";
    errno = 0;
    xlink_channel_t* ch = xlink_open(XLINK_RTSP, rtsp_url, NULL);
    CHECK(ch == NULL, "xlink_open(RTSP) returns NULL");

    /* xlink_errstr(NULL) should return strerror(errno) which is
     * "Function not implemented" for ENOSYS */
    const char* err = xlink_errstr(NULL);
    CHECK(err != NULL, "xlink_errstr(NULL) after RTSP open failure returns string");
    CHECK(strlen(err) > 0, "xlink_errstr(NULL) string is non-empty");
    printf("    errstr = '%s'\n", err);

    /* xlink_type_str(RTSP) should return "unknown" since no backend */
    const char* tstr = xlink_type_str(XLINK_RTSP);
    CHECK_STR(tstr, "unknown");
}

static void test_max_message(void) {
    printf("\n--- SHM max-size message (%d bytes) ---\n", SHM_IPC_MAX_DATA);

    shm_destroy("/xlink_test_max");
    xlink_opt_t opt = XLINK_OPT_DEFAULT;
    opt.flags = XLINK_CREATE;

    xlink_channel_t* tx = xlink_open(XLINK_SHM, "/xlink_test_max", &opt);
    CHECK(tx != NULL, "shm create");

    xlink_channel_t* rx = xlink_open(XLINK_SHM, "/xlink_test_max", NULL);
    CHECK(rx != NULL, "shm open reader");

    if (tx && rx) {
        /* Fill buffer with SHM_IPC_MAX_DATA bytes */
        uint8_t* big = malloc(SHM_IPC_MAX_DATA);
        for (int i = 0; i < SHM_IPC_MAX_DATA; i++)
            big[i] = (uint8_t)(i & 0xff);

        int rc = xlink_send(tx, big, SHM_IPC_MAX_DATA);
        CHECK(rc == 0, "shm send max-size");

        uint8_t* buf = malloc(SHM_IPC_MAX_DATA);
        size_t len = SHM_IPC_MAX_DATA;
        rc = xlink_recv(rx, buf, &len);
        CHECK(rc == 0 && len == SHM_IPC_MAX_DATA, "shm recv max-size");
        CHECK(memcmp(big, buf, SHM_IPC_MAX_DATA) == 0,
              "shm max-size content match");

        free(big);
        free(buf);
    }

    if (tx) xlink_close(tx);
    if (rx) xlink_close(rx);
    shm_destroy("/xlink_test_max");
}

static void test_serial_open_failure(void) {
    printf("\n--- Serial backend: open failure ---\n");

    /* Non-existent serial device should return NULL */
    errno = 0;
    xlink_channel_t* ch = xlink_open(XLINK_SERIAL, "/dev/nonexistent_serial_test", NULL);
    CHECK(ch == NULL, "serial open(nonexistent) returns NULL");
    CHECK(errno != 0, "serial open failure sets errno");

    /* xlink_errstr(NULL) should give error message via errno */
    const char* err = xlink_errstr(NULL);
    CHECK(err != NULL && strlen(err) > 0, "xlink_errstr(NULL) after serial failure is non-empty");
    printf("    errstr = '%s'\n", err);

    /* Invalid baud via opt — should not crash, should fail open on device path */
    errno = 0;
    xlink_opt_t opt = XLINK_OPT_DEFAULT;
    opt.serial.baud = 300;  /* invalid baud (< 1200, gets clamped to 9600) */
    ch = xlink_open(XLINK_SERIAL, "/dev/nonexistent_serial_test:999999", &opt);
    CHECK(ch == NULL, "serial open(invalid baud) returns NULL (device doesn't exist)");
}

static void test_wait_invalid(void) {
    printf("\n--- xlink_wait invalid arguments ---\n");

    int rc;

    /* n == 0 */
    errno = 0;
    rc = xlink_wait(NULL, 0, 100);
    CHECK(rc == -2, "xlink_wait(NULL, 0, 100) returns -2");
    CHECK(errno == EINVAL, "errno is EINVAL for n=0");

    /* n < 0 */
    errno = 0;
    rc = xlink_wait(NULL, -1, 100);
    CHECK(rc == -2, "xlink_wait(NULL, -1, 100) returns -2");
    CHECK(errno == EINVAL, "errno is EINVAL for n=-1");

    /* chans == NULL with positive n */
    errno = 0;
    rc = xlink_wait(NULL, 2, 100);
    CHECK(rc == -2, "xlink_wait(NULL, 2, 100) returns -2");
    CHECK(errno == EINVAL, "errno is EINVAL for chans=NULL");

    /* chans[i] is NULL element in array */
    xlink_channel_t* null_chans[2] = { NULL, NULL };
    errno = 0;
    rc = xlink_wait(null_chans, 2, 100);
    CHECK(rc == -2, "xlink_wait with all-NULL channels returns -2");
    CHECK(errno == EINVAL, "errno is EINVAL for NULL channel element");

    /* Mix of valid and NULL channel */
    const char* pipe_path = "/tmp/xlink_test_err_wait_mix";
    unlink(pipe_path);
    mkfifo(pipe_path, 0666);
    xlink_opt_t opt = XLINK_OPT_DEFAULT;
    opt.flags = XLINK_CREATE;
    xlink_channel_t* valid_ch = xlink_open(XLINK_PIPE, pipe_path, &opt);
    CHECK(valid_ch != NULL, "open pipe for mixed wait test");

    if (valid_ch) {
        xlink_channel_t* mixed[2] = { valid_ch, NULL };
        errno = 0;
        rc = xlink_wait(mixed, 2, 100);
        CHECK(rc == -2, "xlink_wait with NULL in middle returns -2");
        CHECK(errno == EINVAL, "errno is EINVAL for partial NULL channels");

        xlink_close(valid_ch);
    }
    unlink(pipe_path);
}

int main(void) {
    printf("=== xlink Error handling tests ===\n");

    test_null_channel();
    test_invalid_open();
    test_rtsp_not_implemented();
    test_empty_message();
    test_file_write_readonly();
    test_max_message();
    test_serial_open_failure();
    test_wait_invalid();

    printf("\n=== %s ===\n", failures ? "FAILED" : "ALL PASSED");
    return failures ? 1 : 0;
}
