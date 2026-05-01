/*
 * SHM stress test — 10,000 messages, measure throughput & latency.
 *
 * Writer sends, Reader receives in same process via separate channels.
 * Reports: msg/s, avg/min/max latency.
 */

#include "xlink.h"
#include "shm_ipc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char* SHM_NAME = "/xlink_stress_shm";

static long ns_diff(const struct timespec* end,
                    const struct timespec* start) {
    return (end->tv_sec - start->tv_sec) * 1000000000L
         + (end->tv_nsec - start->tv_nsec);
}

int main(void) {
    const int N = 10000;
    const char* msg = "stress test payload 1234567890";
    size_t msglen = strlen(msg) + 1;

    shm_destroy(SHM_NAME);

    printf("=== SHM stress test: %d messages ===\n", N);

    /* Writer */
    xlink_opt_t opt_w = XLINK_OPT_DEFAULT;
    opt_w.flags = XLINK_CREATE;

    xlink_channel_t* tx = xlink_open(XLINK_SHM, SHM_NAME, &opt_w);
    if (!tx) { fprintf(stderr, "FAIL: writer open\n"); return 1; }

    /* Reader */
    xlink_opt_t opt_r = XLINK_OPT_DEFAULT;
    xlink_channel_t* rx = xlink_open(XLINK_SHM, SHM_NAME, &opt_r);
    if (!rx) { fprintf(stderr, "FAIL: reader open\n"); return 1; }

    struct timespec t0, t1;
    long total_ns = 0;
    long min_ns   = 999999999L;
    long max_ns   = 0;
    int  errors   = 0;

    clock_gettime(CLOCK_MONOTONIC, &t0);

    for (int i = 0; i < N; i++) {
        struct timespec ts, tr;

        clock_gettime(CLOCK_MONOTONIC, &ts);
        if (xlink_send(tx, msg, msglen) != 0) {
            fprintf(stderr, "FAIL: send #%d\n", i);
            errors++;
            break;
        }

        uint8_t buf[4096];
        size_t  len = sizeof(buf);
        if (xlink_recv(rx, buf, &len) != 0) {
            fprintf(stderr, "FAIL: recv #%d\n", i);
            errors++;
            break;
        }

        clock_gettime(CLOCK_MONOTONIC, &tr);

        if (len != msglen || memcmp(buf, msg, msglen) != 0) {
            fprintf(stderr, "FAIL: data mismatch #%d\n", i);
            errors++;
            break;
        }

        long lat = ns_diff(&tr, &ts);
        total_ns += lat;
        if (lat < min_ns) min_ns = lat;
        if (lat > max_ns) max_ns = lat;
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);
    long elapsed_ns = ns_diff(&t1, &t0);
    int  passed = N - errors;

    xlink_close(tx);
    xlink_close(rx);
    shm_destroy(SHM_NAME);

    printf("  passed:  %d / %d\n", passed, N);
    printf("  elapsed: %ld ms\n", elapsed_ns / 1000000);
    printf("  rate:    %ld msg/s\n",
           elapsed_ns > 0 ? (long)passed * 1000000000L / elapsed_ns : 0);
    if (passed > 0) {
        printf("  latency: avg %ld us, min %ld us, max %ld us\n",
               total_ns / passed / 1000,
               min_ns / 1000,
               max_ns / 1000);
    }

    printf("=== %s ===\n", errors ? "FAILED" : "ALL PASSED");
    return errors ? 1 : 0;
}
