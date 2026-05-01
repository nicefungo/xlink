/*
 * Serial NONBLOCK flag test via pseudo-terminal.
 *
 * Verifies:
 *   1. NONBLOCK serial with no data → xlink_recv returns -1 immediately
 *   2. NONBLOCK serial with framed data → xlink_recv succeeds
 *   3. xlink_write (raw) with NONBLOCK → succeeds for small writes
 */

#define _GNU_SOURCE
#include "xlink.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/uio.h>
#include <errno.h>

#if defined(__linux__)
#  include <pty.h>
#else
#  include <util.h>
#endif

static int failures = 0;
static int checks   = 0;

#define CHECK(cond, msg) do { \
    checks++; \
    if (!(cond)) { \
        fprintf(stderr, "  FAIL [%d] %s\n", checks, msg); \
        failures++; \
    } else { \
        printf("  PASS [%d] %s\n", checks, msg); \
    } \
} while (0)

static int open_pty_pair(int* master_fd, char* slave_name, size_t sn_sz) {
    *master_fd = posix_openpt(O_RDWR | O_NOCTTY);
    if (*master_fd < 0) { perror("posix_openpt"); return -1; }
    if (grantpt(*master_fd) != 0 || unlockpt(*master_fd) != 0) {
        perror("grantpt/unlockpt");
        close(*master_fd);
        return -1;
    }
    const char* sn = ptsname(*master_fd);
    if (!sn) { perror("ptsname"); close(*master_fd); return -1; }
    size_t n = strlen(sn);
    if (n >= sn_sz) n = sn_sz - 1;
    memcpy(slave_name, sn, n);
    slave_name[n] = '\0';
    return 0;
}

/* Inject a framed message into the master side of the PTY */
static int inject_framed(int master_fd, const void* data, size_t len) {
    uint8_t hdr[4];
    hdr[0] = (uint8_t)(len >> 24);
    hdr[1] = (uint8_t)(len >> 16);
    hdr[2] = (uint8_t)(len >>  8);
    hdr[3] = (uint8_t)(len >>  0);

    struct iovec iov[2];
    iov[0].iov_base = hdr;
    iov[0].iov_len  = 4;
    iov[1].iov_base = (void*)data;
    iov[1].iov_len  = len;

    size_t total = 0;
    while (total < 4 + len) {
        ssize_t n = writev(master_fd, iov, 2);
        if (n < 0) { if (errno == EINTR) continue; return -1; }
        total += (size_t)n;
        if ((size_t)n < iov[0].iov_len) {
            iov[0].iov_base = (char*)iov[0].iov_base + n;
            iov[0].iov_len -= (size_t)n;
        } else {
            size_t rem = (size_t)n - iov[0].iov_len;
            iov[0].iov_len = 0;
            iov[1].iov_base = (char*)iov[1].iov_base + rem;
            iov[1].iov_len -= rem;
        }
    }
    return 0;
}

static void test_serial_nonblock_no_data(void) {
    int master_fd;
    char slave_name[256];

    if (open_pty_pair(&master_fd, slave_name, sizeof(slave_name)) != 0)
        return;

    xlink_opt_t opt = XLINK_OPT_DEFAULT;
    opt.flags = XLINK_CREATE | XLINK_NONBLOCK;

    xlink_channel_t* ch = xlink_open(XLINK_SERIAL, slave_name, &opt);
    CHECK(ch != NULL, "serial NONBLOCK open");

    uint8_t buf[256];
    size_t len = sizeof(buf);
    int rc = xlink_recv(ch, buf, &len);
    CHECK(rc == -1, "serial NONBLOCK recv with no data returns -1");

    xlink_close(ch);
    close(master_fd);
}

static void test_serial_nonblock_with_data(void) {
    int master_fd;
    char slave_name[256];

    if (open_pty_pair(&master_fd, slave_name, sizeof(slave_name)) != 0)
        return;

    xlink_opt_t opt = XLINK_OPT_DEFAULT;
    opt.flags = XLINK_CREATE | XLINK_NONBLOCK;

    xlink_channel_t* ch = xlink_open(XLINK_SERIAL, slave_name, &opt);
    CHECK(ch != NULL, "serial NONBLOCK open (with data)");

    /* Inject framed message via master */
    const char* msg = "NONBLOCK OK";
    size_t msglen = strlen(msg) + 1;
    CHECK(inject_framed(master_fd, msg, msglen) == 0,
          "inject framed message via master");

    /* Give the PTY a moment to propagate */
    usleep(50000);

    uint8_t buf[256];
    size_t len = sizeof(buf);
    int rc = xlink_recv(ch, buf, &len);
    CHECK(rc == 0, "serial NONBLOCK recv succeeds with data");
    CHECK(len == msglen, "serial NONBLOCK recv correct length");
    CHECK(memcmp(buf, msg, msglen) == 0,
          "serial NONBLOCK recv correct content");

    xlink_close(ch);
    close(master_fd);
}

static void test_serial_nonblock_raw_write(void) {
    /* xlink_write on serial goes through serial_backend_send (raw write).
     * NONBLOCK should not prevent small raw writes. */
    int master_fd;
    char slave_name[256];

    if (open_pty_pair(&master_fd, slave_name, sizeof(slave_name)) != 0)
        return;

    xlink_opt_t opt = XLINK_OPT_DEFAULT;
    opt.flags = XLINK_CREATE | XLINK_NONBLOCK;

    xlink_channel_t* ch = xlink_open(XLINK_SERIAL, slave_name, &opt);
    CHECK(ch != NULL, "serial NONBLOCK open (raw write)");

    const char* raw = "raw data";
    size_t rawlen = strlen(raw) + 1;
    int rc = xlink_write(ch, raw, rawlen);
    CHECK(rc == 0, "serial NONBLOCK xlink_write succeeds");

    /* Read raw from master side */
    uint8_t buf[256];
    ssize_t n = read(master_fd, buf, sizeof(buf));
    CHECK(n > 0, "master reads raw data after serial write");
    if (n > 0) {
        CHECK((size_t)n == rawlen, "raw write correct length");
        CHECK(memcmp(buf, raw, rawlen) == 0, "raw write correct content");
    }

    xlink_close(ch);
    close(master_fd);
}

int main(void) {
    printf("=== Serial NONBLOCK tests ===\n");

    test_serial_nonblock_no_data();
    test_serial_nonblock_with_data();
    test_serial_nonblock_raw_write();

    printf("=== RESULTS: %d checks, %d failures ===\n", checks, failures);
    return failures ? 1 : 0;
}
