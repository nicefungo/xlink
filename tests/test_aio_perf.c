/* test_aio_perf.c — async I/O engine performance benchmarks
 *
 * Measures latency and throughput for epoll, poll, and io_uring engines.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "xlink.h"

#define NITER  100

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}

static xlink_opt_t opt_create = { .flags = XLINK_CREATE };
static xlink_opt_t opt_default = { .flags = 0 };

static void banner(const char *title) {
    printf("\n=== %s ===\n", title);
}

/* ─── Benchmark 1: Pipe latency (poll engine) ─── */
static int bench_pipe_poll(void) {
    banner("Pipe latency — poll engine (100 iterations)");

    xlink_channel_t *tx = xlink_open(XLINK_PIPE, "/tmp/xlink_perf_poll", &opt_create);
    xlink_channel_t *rx = xlink_open(XLINK_PIPE, "/tmp/xlink_perf_poll", &opt_default);
    void *aio = xlink_aio_create(1); /* POLL */

    if (!tx || !rx || !aio) {
        printf("  SKIP: setup failed\n");
        if (tx) xlink_close(tx);
        if (rx) xlink_close(rx);
        if (aio) xlink_aio_destroy(aio);
        return 1;
    }

    double t0 = now_ms();
    for (int i = 0; i < NITER; i++) {
        char buf[1024];
        memset(buf, 'A' + (i % 26), sizeof(buf));
        xlink_send(tx, buf, sizeof(buf));
        xlink_channel_t *chans[] = {rx};
        int idx = xlink_wait_aio(chans, 1, 100, aio);
        if (idx < 0) { printf("  FAIL: wait_aio at iter %d\n", i); goto done; }
        size_t len = sizeof(buf);
        int n = xlink_recv(rx, buf, &len);
        if (n < 0 || len != sizeof(buf)) { printf("  FAIL: recv at iter %d\n", i); goto done; }
    }
    double t1 = now_ms();
    double lat = (t1 - t0) / NITER;
    printf("  Poll engine: %.3f ms/round-trip (total %.1f ms for %d iterations)\n",
           lat, t1 - t0, NITER);

done:
    xlink_close(tx);
    xlink_close(rx);
    xlink_aio_destroy(aio);
    return 0;
}

/* ─── Benchmark 2: Pipe latency (epoll engine) ─── */
static int bench_pipe_epoll(void) {
    banner("Pipe latency — epoll engine (100 iterations)");

    xlink_channel_t *tx = xlink_open(XLINK_PIPE, "/tmp/xlink_perf_epoll", &opt_create);
    xlink_channel_t *rx = xlink_open(XLINK_PIPE, "/tmp/xlink_perf_epoll", &opt_default);
    void *aio = xlink_aio_create(2); /* EPOLL */

    if (!tx || !rx || !aio) {
        printf("  SKIP: setup failed\n");
        if (tx) xlink_close(tx);
        if (rx) xlink_close(rx);
        if (aio) xlink_aio_destroy(aio);
        return 1;
    }

    double t0 = now_ms();
    for (int i = 0; i < NITER; i++) {
        char buf[1024];
        memset(buf, 'A' + (i % 26), sizeof(buf));
        xlink_send(tx, buf, sizeof(buf));
        xlink_channel_t *chans[] = {rx};
        int idx = xlink_wait_aio(chans, 1, 100, aio);
        if (idx < 0) { printf("  FAIL: wait_aio at iter %d\n", i); goto done; }
        size_t len = sizeof(buf);
        int n = xlink_recv(rx, buf, &len);
        if (n < 0 || len != sizeof(buf)) { printf("  FAIL: recv at iter %d\n", i); goto done; }
    }
    double t1 = now_ms();
    double lat = (t1 - t0) / NITER;
    printf("  Epoll engine: %.3f ms/round-trip (total %.1f ms for %d iterations)\n",
           lat, t1 - t0, NITER);

done:
    xlink_close(tx);
    xlink_close(rx);
    xlink_aio_destroy(aio);
    return 0;
}

/* ─── Benchmark 3: Pipe throughput (32KB bulk) ─── */
static int bench_pipe_throughput(void) {
    banner("Pipe throughput — bulk 32KB (epoll vs poll)");

    xlink_channel_t *tx = xlink_open(XLINK_PIPE, "/tmp/xlink_perf_bulk", &opt_create);
    xlink_channel_t *rx = xlink_open(XLINK_PIPE, "/tmp/xlink_perf_bulk", &opt_default);

    if (!tx || !rx) {
        printf("  SKIP: setup failed\n");
        if (tx) xlink_close(tx);
        if (rx) xlink_close(rx);
        return 1;
    }

    size_t bufsz = 32 * 1024;  /* 32KB */
    char *buf = malloc(bufsz);
    memset(buf, 'X', bufsz);
    int nrounds = 50;

    /* Poll engine */
    void *aio_poll = xlink_aio_create(1);
    double t0 = now_ms();
    for (int i = 0; i < nrounds; i++) {
        xlink_send(tx, buf, bufsz);
        xlink_channel_t *chans[] = {rx};
        xlink_wait_aio(chans, 1, 100, aio_poll);
        size_t len = bufsz;
        xlink_recv(rx, buf, &len);
    }
    double t1 = now_ms();
    double poll_mbps = (nrounds * bufsz / (1024.0 * 1024.0)) / ((t1 - t0) / 1000.0);
    printf("  Poll:  %.1f MB/s (%.1f ms for %dx %zuKB)\n", poll_mbps, t1 - t0, nrounds, bufsz/1024);
    xlink_aio_destroy(aio_poll);

    /* Epoll engine */
    void *aio_epoll = xlink_aio_create(2);
    t0 = now_ms();
    for (int i = 0; i < nrounds; i++) {
        xlink_send(tx, buf, bufsz);
        xlink_channel_t *chans[] = {rx};
        xlink_wait_aio(chans, 1, 100, aio_epoll);
        size_t len = bufsz;
        xlink_recv(rx, buf, &len);
    }
    t1 = now_ms();
    double epoll_mbps = (nrounds * bufsz / (1024.0 * 1024.0)) / ((t1 - t0) / 1000.0);
    printf("  Epoll: %.1f MB/s (%.1f ms for %dx %zuKB)\n", epoll_mbps, t1 - t0, nrounds, bufsz/1024);
    xlink_aio_destroy(aio_epoll);

    free(buf);
    xlink_close(tx);
    xlink_close(rx);
    return 0;
}

/* ─── Benchmark 4: SHM latency (epoll + eventfd) ─── */
static int bench_shm_latency(void) {
    banner("SHM latency — epoll (100 iterations)");

    xlink_channel_t *tx = xlink_open(XLINK_SHM, "xlink_perf_shm", &opt_create);
    xlink_channel_t *rx = xlink_open(XLINK_SHM, "xlink_perf_shm", &opt_default);
    void *aio = xlink_aio_create(2); /* EPOLL */

    if (!tx || !rx || !aio) {
        printf("  SKIP: setup failed\n");
        if (tx) xlink_close(tx);
        if (rx) xlink_close(rx);
        if (aio) xlink_aio_destroy(aio);
        return 1;
    }

    double t0 = now_ms();
    for (int i = 0; i < NITER; i++) {
        char buf[1024];
        memset(buf, 'A' + (i % 26), sizeof(buf));
        xlink_send(tx, buf, sizeof(buf));
        xlink_channel_t *chans[] = {rx};
        int idx = xlink_wait_aio(chans, 1, 100, aio);
        if (idx < 0) { printf("  FAIL: wait_aio at iter %d\n", i); goto done; }
        size_t len = sizeof(buf);
        int n = xlink_recv(rx, buf, &len);
        if (n < 0 || len != sizeof(buf)) { printf("  FAIL: recv at iter %d\n", i); goto done; }
    }
    double t1 = now_ms();
    double lat = (t1 - t0) / NITER;
    printf("  SHM epoll: %.3f ms/round-trip (total %.1f ms for %d iterations)\n",
           lat, t1 - t0, NITER);

done:
    xlink_close(tx);
    xlink_close(rx);
    xlink_aio_destroy(aio);
    return 0;
}

/* ─── Benchmark 5: Multi-channel (4 pipes, sequential) ─── */
static int bench_multi_channel(void) {
    banner("Multi-channel — 4 pipes, sequential activation (epoll, 50 iterations)");

    xlink_channel_t *tx[4], *rx[4];
    char names[4][64];
    for (int i = 0; i < 4; i++) {
        snprintf(names[i], sizeof(names[i]), "/tmp/xlink_perf_multi_%d", i);
        tx[i] = xlink_open(XLINK_PIPE, names[i], &opt_create);
        rx[i] = xlink_open(XLINK_PIPE, names[i], &opt_default);
        if (!tx[i] || !rx[i]) {
            printf("  SKIP: setup failed for channel %d\n", i);
            for (int j = 0; j <= i; j++) {
                if (tx[j]) xlink_close(tx[j]);
                if (rx[j]) xlink_close(rx[j]);
            }
            return 1;
        }
    }

    void *aio = xlink_aio_create(2); /* EPOLL */
    if (!aio) {
        printf("  SKIP: aio create failed\n");
        return 1;
    }

    double t0 = now_ms();
    for (int iter = 0; iter < 50; iter++) {
        int write_idx = iter % 4;
        char buf[256];
        memset(buf, 'a' + write_idx, sizeof(buf));
        xlink_send(tx[write_idx], buf, sizeof(buf));
        xlink_channel_t *chans[] = {rx[0], rx[1], rx[2], rx[3]};
        int idx = xlink_wait_aio(chans, 4, 100, aio);
        if (idx < 0) { printf("  FAIL: wait_aio at iter %d\n", iter); goto done; }
        if (idx != write_idx) {
            printf("  FAIL: expected channel %d, got %d\n", write_idx, idx);
            goto done;
        }
        size_t len = sizeof(buf);
        int n = xlink_recv(rx[idx], buf, &len);
        if (n < 0 || len != sizeof(buf)) { printf("  FAIL: recv at iter %d\n", iter); goto done; }
    }
    double t1 = now_ms();
    printf("  Multi-channel epoll: %.3f ms/round-trip (total %.1f ms for 50 iterations)\n",
           (t1 - t0) / 50, t1 - t0);

done:
    for (int i = 0; i < 4; i++) {
        xlink_close(tx[i]);
        xlink_close(rx[i]);
    }
    xlink_aio_destroy(aio);
    return 0;
}

/* ─── Benchmark 6: io_uring engine (if available) ─── */
static int bench_io_uring(void) {
    banner("io_uring engine — pipe latency (100 iterations)");

    xlink_channel_t *tx = xlink_open(XLINK_PIPE, "/tmp/xlink_perf_uring", &opt_create);
    xlink_channel_t *rx = xlink_open(XLINK_PIPE, "/tmp/xlink_perf_uring", &opt_default);
    void *aio = xlink_aio_create(3); /* IO_URING */

    if (!tx || !rx || !aio) {
        printf("  SKIP: io_uring not available on this kernel\n");
        if (tx) xlink_close(tx);
        if (rx) xlink_close(rx);
        if (aio) xlink_aio_destroy(aio);
        return 0;
    }

    printf("  Engine: %s\n", xlink_aio_name(aio));

    double t0 = now_ms();
    for (int i = 0; i < NITER; i++) {
        char buf[1024];
        memset(buf, 'A' + (i % 26), sizeof(buf));
        xlink_send(tx, buf, sizeof(buf));
        xlink_channel_t *chans[] = {rx};
        int idx = xlink_wait_aio(chans, 1, 100, aio);
        if (idx < 0) { printf("  FAIL: wait_aio at iter %d\n", i); goto done; }
        size_t len = sizeof(buf);
        int n = xlink_recv(rx, buf, &len);
        if (n < 0 || len != sizeof(buf)) { printf("  FAIL: recv at iter %d\n", i); goto done; }
    }
    double t1 = now_ms();
    double lat = (t1 - t0) / NITER;
    printf("  io_uring: %.3f ms/round-trip (total %.1f ms for %d iterations)\n",
           lat, t1 - t0, NITER);

done:
    xlink_close(tx);
    xlink_close(rx);
    xlink_aio_destroy(aio);
    return 0;
}

/* ─── Main ─── */
int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("=== xlink async I/O performance benchmarks ===\n");

    bench_pipe_poll();
    bench_pipe_epoll();
    bench_pipe_throughput();
    bench_shm_latency();
    bench_multi_channel();
    bench_io_uring();

    printf("\n=== Benchmarks complete ===\n");
    return 0;
}