/*
 * Basic SHM round-trip test via xlink API.
 *
 * Writer sends a message, Reader receives and verifies.
 * Both use the unified xlink_open/send/recv interface.
 */

#include "xlink.h"
#include "shm_ipc.h"   /* for shm_destroy cleanup */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int test_roundtrip(void) {
    const char* name = "/xlink_test_shm";
    const char* msg  = "Hello from xlink SHM test!";
    size_t      msglen = strlen(msg) + 1;  /* include null */

    /* Remove stale SHM (ignore error — may not exist) */
    shm_destroy(name);

    /* ── Writer (sender): create SHM, send message ── */
    xlink_opt_t opt = XLINK_OPT_DEFAULT;
    opt.flags = XLINK_CREATE;   /* init segment */

    xlink_channel_t* tx = xlink_open(XLINK_SHM, name, &opt);
    if (!tx) {
        fprintf(stderr, "FAIL: tx open\n");
        return 1;
    }

    if (xlink_send(tx, msg, msglen) != 0) {
        fprintf(stderr, "FAIL: send: %s\n", xlink_errstr(tx));
        xlink_close(tx);
        return 1;
    }

    /* ── Reader: open same segment, receive ── */
    xlink_opt_t opt_r = XLINK_OPT_DEFAULT;
    /* no XLINK_CREATE — segment already exists */

    xlink_channel_t* rx = xlink_open(XLINK_SHM, name, &opt_r);
    if (!rx) {
        fprintf(stderr, "FAIL: rx open\n");
        xlink_close(tx);
        return 1;
    }

    uint8_t buf[4096];
    size_t  len = sizeof(buf);
    if (xlink_recv(rx, buf, &len) != 0) {
        fprintf(stderr, "FAIL: recv: %s\n", xlink_errstr(rx));
        xlink_close(tx);
        xlink_close(rx);
        return 1;
    }

    if (len != msglen || memcmp(buf, msg, msglen) != 0) {
        fprintf(stderr, "FAIL: data mismatch (got %zu bytes, expected %zu)\n",
                len, msglen);
        xlink_close(tx);
        xlink_close(rx);
        return 1;
    }

    xlink_close(tx);
    xlink_close(rx);

    /* Cleanup */
    shm_destroy(name);

    printf("  SHM round-trip: %zu bytes OK\n", len);
    return 0;
}

/*
 * Test non-blocking recv on empty SHM queue:
 * open with XLINK_NONBLOCK, recv when no data → -1 with "no data" errstr.
 */
static int test_nonblock_empty(void) {
    const char* name = "/xlink_test_shm_nb";

    shm_destroy(name);

    /* Writer: create SHM segment */
    xlink_opt_t opt = XLINK_OPT_DEFAULT;
    opt.flags = XLINK_CREATE;
    xlink_channel_t* tx = xlink_open(XLINK_SHM, name, &opt);
    if (!tx) {
        fprintf(stderr, "FAIL: tx open (nonblock test)\n");
        return 1;
    }

    /* Reader: open with XLINK_NONBLOCK, no CREATE */
    xlink_opt_t opt_r = XLINK_OPT_DEFAULT;
    opt_r.flags = XLINK_NONBLOCK;
    xlink_channel_t* rx = xlink_open(XLINK_SHM, name, &opt_r);
    if (!rx) {
        fprintf(stderr, "FAIL: rx open (nonblock test)\n");
        xlink_close(tx);
        return 1;
    }

    int failures = 0;

    /* Recv on empty queue → should fail with "no data" */
    uint8_t buf[64];
    size_t len = sizeof(buf);
    int rc = xlink_recv(rx, buf, &len);
    if (rc == 0) {
        fprintf(stderr, "FAIL: nonblock recv on empty should fail\n");
        failures++;
    } else {
        const char* err = xlink_errstr(rx);
        if (strstr(err, "no data") != NULL) {
            printf("  PASS: nonblock recv empty returns 'no data'\n");
        } else {
            fprintf(stderr, "FAIL: expected 'no data', got '%s'\n", err);
            failures++;
        }
    }

    /* Now send data, recv should succeed */
    const char* msg = "nonblock test";
    if (xlink_send(tx, msg, strlen(msg) + 1) != 0) {
        fprintf(stderr, "FAIL: send (nonblock test): %s\n", xlink_errstr(tx));
        failures++;
    }

    len = sizeof(buf);
    rc = xlink_recv(rx, buf, &len);
    if (rc != 0) {
        fprintf(stderr, "FAIL: recv after send: %s\n", xlink_errstr(rx));
        failures++;
    } else if (len != strlen(msg) + 1 || memcmp(buf, msg, len) != 0) {
        fprintf(stderr, "FAIL: data mismatch\n");
        failures++;
    } else {
        printf("  PASS: nonblock recv gets sent data\n");
    }

    xlink_close(tx);
    xlink_close(rx);
    shm_destroy(name);
    return failures;
}

int main(void) {
    int failures = 0;
    printf("=== xlink SHM test ===\n");
    failures += test_roundtrip();
    failures += test_nonblock_empty();
    printf("=== %s ===\n", failures ? "FAILED" : "ALL PASSED");
    return failures ? 1 : 0;
}
