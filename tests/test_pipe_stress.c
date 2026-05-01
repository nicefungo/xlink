/*
 * Pipe stress test: send N messages in sequence over a named pipe.
 * Tests framing layer handling of multiple messages and large payloads.
 */

#include "xlink.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#define FIFO_PATH "/tmp/xlink_pipe_stress"
#define NMESSAGES 500

static int failures = 0;
#define CHECK(cond, msg) do {                                      \
    if (!(cond)) {                                                  \
        fprintf(stderr, "  FAIL: %s\n", msg);                       \
        failures++;                                                 \
    } else {                                                        \
        fprintf(stderr, "  PASS: %s\n", msg);                       \
    }                                                               \
} while(0)

int main(void) {
    unlink(FIFO_PATH);

    printf("=== Pipe multi-message stress test ===\n");

    xlink_opt_t opt = XLINK_OPT_DEFAULT;
    opt.flags = XLINK_CREATE;

    xlink_channel_t* ch = xlink_open(XLINK_PIPE, FIFO_PATH, &opt);
    CHECK(ch != NULL, "open pipe with CREATE");

    if (!ch) { unlink(FIFO_PATH); return 1; }

    /* Send NMESSAGES messages of varying sizes */
    for (int i = 0; i < NMESSAGES; i++) {
        size_t msglen = (size_t)((i % 200) + 1);  /* 1..200 bytes */
        uint8_t* msg = malloc(msglen);
        if (!msg) { fprintf(stderr, "malloc\n"); return 1; }
        memset(msg, (uint8_t)(i & 0xFF), msglen);

        int rc = xlink_send(ch, msg, msglen);
        free(msg);
        if (rc != 0) {
            fprintf(stderr, "  FAIL: send #%d: %s\n", i, xlink_errstr(ch));
            failures++;
            goto done;
        }

        uint8_t buf[4096];
        size_t len = sizeof(buf);
        rc = xlink_recv(ch, buf, &len);
        if (rc != 0) {
            fprintf(stderr, "  FAIL: recv #%d: %s\n", i, xlink_errstr(ch));
            failures++;
            goto done;
        }

        if (len != msglen) {
            fprintf(stderr, "  FAIL: #%d len mismatch (got %zu, exp %zu)\n",
                    i, len, msglen);
            failures++;
            goto done;
        }

        /* Verify content */
        for (size_t j = 0; j < len; j++) {
            if (buf[j] != (uint8_t)(i & 0xFF)) {
                fprintf(stderr, "  FAIL: #%d byte %zu mismatch\n", i, j);
                failures++;
                goto done;
            }
        }
    }

    CHECK(failures == 0, "all NMESSAGES sent and received correctly");

    /* Large message test */
    {
        const size_t large = 32768;
        uint8_t* big = malloc(large);
        if (!big) { fprintf(stderr, "malloc big\n"); return 1; }
        memset(big, 0xAB, large);

        int rc = xlink_send(ch, big, large);
        if (rc != 0) {
            fprintf(stderr, "  FAIL: large send\n");
            free(big);
            goto done;
        }

        uint8_t* rbuf = malloc(large + 16);
        size_t rlen = large + 16;
        rc = xlink_recv(ch, rbuf, &rlen);
        if (rc != 0) {
            fprintf(stderr, "  FAIL: large recv: %s\n", xlink_errstr(ch));
            failures++;
        } else if (rlen != large) {
            fprintf(stderr, "  FAIL: large len mismatch (%zu vs %zu)\n", rlen, large);
            failures++;
        } else if (memcmp(big, rbuf, large) != 0) {
            fprintf(stderr, "  FAIL: large content mismatch\n");
            failures++;
        } else {
            fprintf(stderr, "  PASS: pipe send/recv 32768 bytes\n");
        }
        free(big);
        free(rbuf);
    }

done:
    xlink_close(ch);
    unlink(FIFO_PATH);
    printf("=== %s ===\n", failures ? "FAILED" : "ALL PASSED");
    return failures ? 1 : 0;
}
