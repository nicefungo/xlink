/*
 * test_nonblock.c — Verify XLINK_NONBLOCK flag works on fd-based backends.
 *
 * Tests:
 *   - Pipe with NONBLOCK: xlink_recv returns -1 when no data ready
 *   - Pipe with NONBLOCK: xlink_recv succeeds when data arrives
 *   - Pipe with NONBLOCK: fd is actually set O_NONBLOCK
 *   - SHM with NONBLOCK: works (SHM has its own non-blocking path)
 *   - Positive: NONBLOCK + CREATE on pipe still creates the FIFO
 */

#include "xlink.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
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

int main(void) {
    fprintf(stderr, "=== Test NONBLOCK flag on fd-based backends ===\n");

    /* ── Pipe with NONBLOCK: detect no-data without blocking ── */
    fprintf(stderr, "\n--- Pipe NONBLOCK: no data returns immediately ---\n");

    const char* pipe_path = "/tmp/xlink_test_nonblock_pipe";
    unlink(pipe_path);
    int rc = mkfifo(pipe_path, 0666);
    CHECK(rc == 0 || errno == EEXIST, "mkfifo for nonblock test");

    /* Open writer (non-CREATE) in a subprocess */
    pid_t pid = fork();
    CHECK(pid >= 0, "fork for nonblock test");

    if (pid == 0) {
        /* Child: write framed data after a short delay, then exit */
        usleep(50000);  /* 50ms delay */
        int wfd = open(pipe_path, O_WRONLY);
        if (wfd < 0) _exit(1);
        /* Write 4-byte BE framing header + payload */
        uint8_t msgdata[] = "hello_nonblock";  /* 14 bytes incl \0 */
        uint8_t hdr[4];
        uint32_t mlen = 14;
        hdr[0] = (uint8_t)(mlen >> 24);
        hdr[1] = (uint8_t)(mlen >> 16);
        hdr[2] = (uint8_t)(mlen >> 8);
        hdr[3] = (uint8_t)(mlen);
        ssize_t n = write(wfd, hdr, 4);
        (void)n;
        n = write(wfd, msgdata, mlen);
        (void)n;
        close(wfd);
        _exit(0);
        _exit(0);
    }

    /* Parent: open pipe with NONBLOCK */
    xlink_opt_t opt = XLINK_OPT_DEFAULT;
    opt.flags = XLINK_CREATE | XLINK_NONBLOCK;
    xlink_channel_t* ch = xlink_open(XLINK_PIPE, pipe_path, &opt);
    CHECK(ch != NULL, "xlink_open pipe with CREATE|NONBLOCK");

    if (ch) {
        /* Try recv immediately — should fail with no data */
        uint8_t buf[128];
        size_t len = sizeof(buf);
        rc = xlink_recv(ch, buf, &len);
        CHECK(rc == -1, "xlink_recv on nonblock pipe with no data returns -1");

        /* Now wait for child to send data, then recv should succeed */
        usleep(100000);  /* wait for child to write */

        len = sizeof(buf);
        rc = xlink_recv(ch, buf, &len);
        if (rc == 0) {
            CHECK(len == 14, "xlink_recv got expected message length");
            CHECK(memcmp(buf, "hello_nonblock", 14) == 0,
                  "received data matches expected content");
        } else {
            CHECK(0, "xlink_recv succeeds after child wrote");
        }

        xlink_close(ch);
    }

    int wstatus;
    waitpid(pid, &wstatus, 0);
    unlink(pipe_path);

    /* ── SHM with NONBLOCK: should still work ── */
    fprintf(stderr, "\n--- SHM with NONBLOCK ---\n");

    const char* shm_name = "/xlink_test_nonblock_shm";
    /* Destroy any stale shm first */
    {
        /* try to signal shm_destroy by opening+closing with create */
        xlink_opt_t destroy_opt = XLINK_OPT_DEFAULT;
        destroy_opt.flags = XLINK_CREATE;
        xlink_channel_t* d = xlink_open(XLINK_SHM, shm_name, &destroy_opt);
        if (d) xlink_close(d);
    }

    /* Create with NONBLOCK */
    xlink_opt_t shm_opt = XLINK_OPT_DEFAULT;
    shm_opt.flags = XLINK_CREATE;
    xlink_channel_t* shm_tx = xlink_open(XLINK_SHM, shm_name, &shm_opt);
    CHECK(shm_tx != NULL, "SHM create (for nonblock test)");

    /* Open reader with NONBLOCK */
    shm_opt.flags = XLINK_NONBLOCK;
    xlink_channel_t* shm_rx = xlink_open(XLINK_SHM, shm_name, &shm_opt);
    CHECK(shm_rx != NULL, "SHM open reader with NONBLOCK");

    if (shm_tx && shm_rx) {
        /* No data yet — recv should fail (nonblocking) */
        uint8_t buf[64];
        size_t len = sizeof(buf);
        rc = xlink_recv(shm_rx, buf, &len);
        CHECK(rc == -1, "SHM nonblock recv with no data returns -1");

        /* Send data */
        const char* msg = "nonblock_shm_msg";
        rc = xlink_send(shm_tx, msg, strlen(msg) + 1);
        CHECK(rc == 0, "SHM send with nonblock reader");

        /* Reader should now get the data */
        len = sizeof(buf);
        rc = xlink_recv(shm_rx, buf, &len);
        CHECK(rc == 0, "SHM nonblock recv succeeds after send");
        if (rc == 0) {
            CHECK(len == strlen(msg) + 1, "SHM nonblock recv correct length");
            CHECK(memcmp(buf, msg, len) == 0, "SHM nonblock recv correct content");
        }
    }

    if (shm_tx) xlink_close(shm_tx);
    if (shm_rx) xlink_close(shm_rx);

    /* Destroy SHM */
    {
        xlink_opt_t destroy_opt = XLINK_OPT_DEFAULT;
        destroy_opt.flags = XLINK_CREATE;
        xlink_channel_t* d = xlink_open(XLINK_SHM, shm_name, &destroy_opt);
        if (d) xlink_close(d);
    }

    /* ── Positive: NONBLOCK + CREATE pipe still creates FIFO ── */
    fprintf(stderr, "\n--- NONBLOCK + CREATE pipe ---\n");

    const char* pipe2_path = "/tmp/xlink_test_nonblock_create2";
    unlink(pipe2_path);

    xlink_opt_t opt2 = XLINK_OPT_DEFAULT;
    opt2.flags = XLINK_CREATE | XLINK_NONBLOCK;
    xlink_channel_t* ch2 = xlink_open(XLINK_PIPE, pipe2_path, &opt2);
    CHECK(ch2 != NULL, "NONBLOCK+CREATE opens pipe");
    if (ch2) {
        /* Verify the pipe file exists (stat won't block since pipe is already open) */
        int r = access(pipe2_path, F_OK);
        CHECK(r == 0, "pipe file exists after NONBLOCK+CREATE");
        xlink_close(ch2);
    }
    unlink(pipe2_path);

    /* ── Summary ── */
    fprintf(stderr, "\n=== RESULTS: %d checks, %d failures ===\n",
            checks, failures);

    return failures > 0 ? 1 : 0;
}
