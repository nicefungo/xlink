/*
 * test_api_util.c — Test xlink utility API functions.
 *
 * Covers:
 *   - xlink_type_str() for all transport types
 *   - xlink_type_str() for invalid type
 *   - xlink_dump() on a live pipe channel (writes to /dev/null)
 *   - xlink_dump() with NULL/invalid state (errno path via no-such shm)
 *   - xlink_peek() on SHM with available data
 */

#include "xlink.h"
#include "shm_ipc.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>

static int failures = 0;
static int checks   = 0;

#define CHECK(cond, msg) do {                                   \
    checks++;                                                   \
    if (!(cond)) {                                              \
        fprintf(stderr, "  FAIL [%d] %s\n", checks, msg);       \
        failures++;                                             \
    } else {                                                    \
        fprintf(stderr, "  PASS [%d] %s\n", checks, msg);      \
    }                                                           \
} while(0)

#define CHECK_STR(actual, expected, msg) do {                   \
    checks++;                                                   \
    if (strcmp(actual, expected) != 0) {                        \
        fprintf(stderr, "  FAIL [%d] %s: got '%s', expected '%s'\n", \
                checks, msg, actual, expected);                 \
        failures++;                                             \
    } else {                                                    \
        fprintf(stderr, "  PASS [%d] %s\n", checks, msg);      \
    }                                                           \
} while(0)

int main(void) {
    fprintf(stderr, "=== Test xlink utility API ===\n");

    /* ── xlink_type_str coverage ── */

    fprintf(stderr, "\n--- xlink_type_str ---\n");

    CHECK_STR(xlink_type_str(XLINK_SHM),     "shm",    "XLINK_SHM → 'shm'");
    CHECK_STR(xlink_type_str(XLINK_PIPE),    "pipe",   "XLINK_PIPE → 'pipe'");
    CHECK_STR(xlink_type_str(XLINK_TCP),     "tcp",    "XLINK_TCP → 'tcp'");
    CHECK_STR(xlink_type_str(XLINK_UDP),     "udp",    "XLINK_UDP → 'udp'");
    CHECK_STR(xlink_type_str(XLINK_SERIAL),  "serial", "XLINK_SERIAL → 'serial'");
    CHECK_STR(xlink_type_str(XLINK_RTSP),    "unknown","XLINK_RTSP → 'unknown' (no backend)");
    CHECK_STR(xlink_type_str(XLINK_FILE),    "file",   "XLINK_FILE → 'file'");

    /* Invalid type: pass a value that no backend handles */
    CHECK_STR(xlink_type_str((xlink_type_t)255), "unknown", "invalid type 255 → 'unknown'");

    /* ── xlink_dump coverage ── */

    fprintf(stderr, "\n--- xlink_dump ---\n");

    /* Dump to /dev/null: just verify it doesn't crash */
    {
        int null_fd = open("/dev/null", O_WRONLY);
        CHECK(null_fd >= 0, "open /dev/null for dump");

        /* Create a named pipe for the channel */
        const char* pipe_path = "/tmp/xlink_test_api_util_pipe";
        unlink(pipe_path);
        int rc = mkfifo(pipe_path, 0666);
        CHECK(rc == 0 || errno == EEXIST, "mkfifo for dump test");

        xlink_opt_t opt = XLINK_OPT_DEFAULT;
        opt.flags = XLINK_CREATE;
        xlink_channel_t* ch = xlink_open(XLINK_PIPE, pipe_path, &opt);
        CHECK(ch != NULL, "xlink_open pipe for dump test");

        if (ch) {
            /* xlink_dump should not crash or produce garbage */
            xlink_dump(ch, null_fd);
            CHECK(1, "xlink_dump to /dev/null (no crash)");

            xlink_close(ch);
        }

        /* Test xlink_open with invalid type (no XLINK_RTSP backend) */
        {
            xlink_channel_t* bad = xlink_open(XLINK_RTSP, "rtsp://invalid", NULL);
            CHECK(bad == NULL, "xlink_open with unsupported type returns NULL");
            if (bad == NULL) {
                const char* err = xlink_errstr(NULL);
                CHECK(err != NULL && strlen(err) > 0, "xlink_errstr(NULL) returns non-empty");
                fprintf(stderr, "       errstr = '%s'\n", err);
            }
        }

        /* Test open nonexistent file without CREATE (should fail) */
        {
            xlink_channel_t* bad = xlink_open(XLINK_FILE, "/nonexistent_path_xyz_abc_test", NULL);
            CHECK(bad == NULL, "xlink_open nonexistent file returns NULL");
            if (bad == NULL) {
                const char* err = xlink_errstr(NULL);
                CHECK(err != NULL && strlen(err) > 0, "xlink_errstr(NULL) on file error");
                fprintf(stderr, "       errstr = '%s'\n", err);
            }
        }

        /* xlink_type_str for a valid pipe channel */
        xlink_opt_t opt2 = XLINK_OPT_DEFAULT;
        opt2.flags = XLINK_CREATE;
        xlink_channel_t* ch2 = xlink_open(XLINK_PIPE, pipe_path, &opt2);
        if (ch2) {
            CHECK_STR(xlink_type_str(XLINK_PIPE), "pipe", "type_str(PIPE) on live channel");
            xlink_dump(ch2, null_fd);
            CHECK(1, "xlink_dump live pipe to /dev/null");
            xlink_close(ch2);
        }

        /* xlink_dump(NULL) should not segfault */
        int null_fd2 = open("/dev/null", O_WRONLY);
        if (null_fd2 >= 0) {
            xlink_dump(NULL, null_fd2);
            CHECK(1, "xlink_dump(NULL) does not segfault");
            close(null_fd2);
        }

        close(null_fd);
        unlink(pipe_path);
    }

    /* ── xlink_peek on unsupported backend ── */
    {
        fprintf(stderr, "\n--- xlink_peek unsupported ---\n");
        const char* pipe_path = "/tmp/xlink_test_api_util_peek";
        unlink(pipe_path);
        mkfifo(pipe_path, 0666);

        xlink_opt_t opt = XLINK_OPT_DEFAULT;
        opt.flags = XLINK_CREATE;
        xlink_channel_t* ch = xlink_open(XLINK_PIPE, pipe_path, &opt);
        CHECK(ch != NULL, "xlink_open pipe for peek test");
        if (ch) {
            size_t avail = 999;
            int rc = xlink_peek(ch, &avail);
            CHECK(rc == 0, "xlink_peek on pipe (unsupported) returns 0");
            CHECK(avail == 0, "xlink_peek sets *avail = 0 on unsupported backend");
            xlink_close(ch);
        }
        unlink(pipe_path);
    }

    /* ── xlink_peek on SHM with data ── */
    {
        fprintf(stderr, "\n--- xlink_peek on SHM (with data) ---\n");
        const char* shm_name = "/xlink_api_util_peek_test";
        shm_destroy(shm_name);

        xlink_opt_t opt = XLINK_OPT_DEFAULT;
        opt.flags = XLINK_CREATE;
        xlink_channel_t* tx = xlink_open(XLINK_SHM, shm_name, &opt);
        CHECK(tx != NULL, "open SHM for peek test (sender)");

        xlink_channel_t* rx = xlink_open(XLINK_SHM, shm_name, NULL);
        CHECK(rx != NULL, "open SHM for peek test (receiver)");

        if (tx && rx) {
            /* Peek before any data: should report 0 available */
            size_t avail = 999;
            int rc = xlink_peek(rx, &avail);
            CHECK(rc == 0, "xlink_peek on empty SHM returns 0");
            CHECK(avail == 0, "xlink_peek on empty SHM reports 0 available");

            /* Send data, then peek */
            const char* msg = "peek-test-data";
            xlink_send(tx, msg, strlen(msg) + 1);

            avail = 999;
            rc = xlink_peek(rx, &avail);
            CHECK(rc == 0, "xlink_peek on SHM with data returns 0");
            CHECK(avail == 15, "xlink_peek on SHM reports 15 bytes available");

            /* Consume data and verify peek drops to 0 */
            uint8_t buf[128];
            size_t len = sizeof(buf);
            rc = xlink_recv(rx, buf, &len);
            CHECK(rc == 0 && len == 15, "read peeked data from SHM");

            avail = 999;
            rc = xlink_peek(rx, &avail);
            CHECK(rc == 0, "xlink_peek after consuming data returns 0");
            CHECK(avail == 0, "xlink_peek after consuming reports 0 available");
        }

        if (tx) xlink_close(tx);
        if (rx) xlink_close(rx);
        shm_destroy(shm_name);
    }

    /* ── Summary ── */
    fprintf(stderr, "\n=== RESULTS: %d checks, %d failures ===\n",
            checks, failures);

    return failures > 0 ? 1 : 0;
}
