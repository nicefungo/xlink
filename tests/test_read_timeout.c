/* test_read_timeout.c — verify xlink_read() timeout behavior.
 * Ensures xlink_read() returns -1 with ETIMEDOUT when no data
 * arrives within the specified timeout.
 *
 * Backends tested: Pipe (has ch->fd, poll-based .read implemented)
 */

#include "xlink.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>

static int nfail = 0;
#define CHECK(cond, fmt, ...) do { \
    if (!(cond)) { \
        printf("  FAIL: " fmt "\n", ##__VA_ARGS__); \
        nfail++; \
    } else { \
        printf("  PASS: " fmt "\n", ##__VA_ARGS__); \
    } \
} while(0)

int main(void) {
    printf("=== xlink_read timeout tests ===\n");

    xlink_opt_t opt = XLINK_OPT_DEFAULT;
    opt.flags |= XLINK_CREATE;

    /* ── Test 1: Pipe, timeout_ms=1 (short timeout, no data) ── */
    printf("\n--- pipe short timeout ---\n");
    xlink_channel_t* a = xlink_open(XLINK_PIPE, "test-read-timeout-a", &opt);
    CHECK(a != NULL, "pipe open succeeds");
    if (!a) return 1;

    char buf[64];
    errno = 0;
    int n = xlink_read(a, buf, sizeof(buf), 1);
    CHECK(n < 0, "xlink_read returns -1 on timeout (got %d)", n);
    CHECK(errno == ETIMEDOUT,
          "errno == ETIMEDOUT after timeout (got %d: %s)", errno, strerror(errno));

    xlink_close(a);
    unlink("test-read-timeout-a");

    /* ── Test 2: Pipe, timeout_ms=0 (non-blocking poll) ── */
    printf("\n--- pipe zero timeout (non-blocking) ---\n");
    a = xlink_open(XLINK_PIPE, "test-read-timeout-b", &opt);
    CHECK(a != NULL, "pipe open for zero timeout");
    if (!a) return 1;

    errno = 0;
    n = xlink_read(a, buf, sizeof(buf), 0);
    CHECK(n < 0, "xlink_read returns -1 with 0 timeout (got %d)", n);
    CHECK(errno == ETIMEDOUT,
          "errno == ETIMEDOUT with 0 timeout (got %d: %s)", errno, strerror(errno));

    xlink_close(a);
    unlink("test-read-timeout-b");

    /* ── Test 3: Pipe, timeout_ms=-1 (blocking, data available) ── */
    printf("\n--- pipe blocking (-1) with data ---\n");
    a = xlink_open(XLINK_PIPE, "test-read-timeout-c", &opt);
    CHECK(a != NULL, "pipe open for blocking test");
    if (!a) return 1;

    const char* msg = "hello timeout test";
    int rc = xlink_write(a, msg, strlen(msg));
    CHECK(rc == 0, "xlink_write succeeds");

    errno = 0;
    n = xlink_read(a, buf, sizeof(buf), -1);
    CHECK(n == (int)strlen(msg), "xlink_read returns correct length (got %d)", n);
    CHECK(memcmp(buf, msg, (size_t)n) == 0, "xlink_read content matches");

    xlink_close(a);
    unlink("test-read-timeout-c");

    printf("\n=== %s ===\n", nfail ? "SOME TESTS FAILED" : "ALL PASSED");
    return nfail ? 1 : 0;
}
