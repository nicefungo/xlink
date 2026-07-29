/*
 * test_zc_perf.c — Zero-Copy performance benchmark
 *
 * Measures send-side throughput of xlink_send() vs xlink_send_zc() for
 * SHM SPSC and TCP channels.
 *
 * For ZC paths: polls completions periodically during sending (every
 * 64 sends) because the completion ring has limited capacity.
 */
#include "xlink.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>

#define SHM_N_ITER   2000
#define TCP_N_ITER   1000
#define MSG_SMALL    256
#define MSG_LARGE    65536
#define BENCH_PORT   20200

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static int checks = 0, fails = 0;

#define CHECK(cond, msg) do { \
    checks++; \
    printf("  %s: %s\n", (cond) ? "PASS" : "FAIL", msg); \
    if (!(cond)) fails++; \
} while(0)

static double calc_mbps(int n, int sz, double ms) {
    return ms > 0 ? (double)(n * sz) / (1024.0 * 1024.0) / (ms / 1000.0) : 0;
}

/* ──── SHM ──────────────────────────────────── */

static void shm_drain_loop(void) {
    xlink_channel_t *ch = xlink_open(XLINK_SHM, "perf_bm", &(xlink_opt_t){0});
    if (!ch) _exit(1);
    for (;;) {
        void *data = NULL;
        size_t len = 0;
        if (xlink_recv_zc(ch, &data, &len) == 0)
            xlink_recv_zc_done(ch, data);
        else
            usleep(10);
    }
}

/*
 * Send `n` messages and periodically drain ZC completions.
 * The ZC completion ring has XLINK_ZC_DONE_CAP=64 slots, so we must
 * poll every ~50 sends to avoid overwriting un-drained entries.
 */
static int shm_send_bench(xlink_channel_t *ch, int n, int sz, int zc) {
    char *buf = calloc(1, sz);
    memset(buf, 'X', sz);
    xlink_zc_buf_t zc_buf = { .addr = buf, .len = sz, .fd = -1, .tag = 0 };

    int zc_pending = 0;
    double t0 = now_ms();
    for (int i = 0; i < n; i++) {
        if (zc) {
            zc_buf.tag = i;
            if (xlink_send_zc(ch, &zc_buf, NULL, NULL) == 0)
                zc_pending++;
            /* Drain every 50 to avoid ring overflow (capacity=64) */
            if (zc_pending >= 50) {
                xlink_zc_poll(ch);
                zc_pending = 0;
            }
        } else {
            xlink_send(ch, buf, sz);
        }
    }
    /* Drain remaining */
    if (zc && zc_pending > 0)
        xlink_zc_poll(ch);
    double t1 = now_ms();

    printf(" %.2f ms (%.1f MB/s)\n", t1 - t0, calc_mbps(n, sz, t1 - t0));
    CHECK(t1 - t0 > 0, "completed");

    free(buf);
    return 0;
}

static int shm_bench(int n, int sz, int zc) {
    printf("  SHM %s %d×%dB:", zc ? "send_zc" : "send   ", n, sz);

    xlink_channel_t *ch = xlink_open(XLINK_SHM, "perf_bm",
        &(xlink_opt_t){.flags = XLINK_CREATE | XLINK_SPSC});
    if (!ch) { CHECK(0, "open"); return 1; }

    pid_t pid = fork();
    if (pid < 0) { xlink_close(ch); return 1; }
    if (pid == 0) {
        xlink_close(ch);
        shm_drain_loop();
    }

    int rc = shm_send_bench(ch, n, sz, zc);
    xlink_close(ch);
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    return rc;
}

/* ──── TCP ──────────────────────────────────── */

static int tcp_echo(int port) {
    signal(SIGPIPE, SIG_IGN);
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port = htons(port);
    int r = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &r, sizeof(r));
    if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) _exit(1);
    listen(lfd, 1);

    struct sockaddr_in cli = {0};
    socklen_t cli_len = sizeof(cli);
    int cfd = accept(lfd, (struct sockaddr *)&cli, &cli_len);
    if (cfd < 0) _exit(1);

    char buf[65536 + 4];
    for (;;) {
        uint32_t len_be = 0;
        if (read(cfd, &len_be, 4) != 4) break;
        uint32_t len = ntohl(len_be);
        ssize_t remain = len;
        while (remain > 0) {
            ssize_t nr = read(cfd, buf,
                remain > (ssize_t)sizeof(buf) ? (ssize_t)sizeof(buf) : remain);
            if (nr <= 0) goto done;
            remain -= nr;
        }
    }
done:
    close(cfd);
    close(lfd);
    _exit(0);
}

static int tcp_send_bench(xlink_channel_t *ch, int n, int sz, int zc) {
    char *buf = calloc(1, sz);
    memset(buf, 'T', sz);
    xlink_zc_buf_t zc_buf = { .addr = buf, .len = sz, .fd = -1, .tag = 0 };

    int zc_pending = 0;
    double t0 = now_ms();
    for (int i = 0; i < n; i++) {
        if (zc) {
            zc_buf.tag = i;
            if (xlink_send_zc(ch, &zc_buf, NULL, NULL) == 0)
                zc_pending++;
            if (zc_pending >= 50) {
                xlink_zc_poll(ch);
                zc_pending = 0;
            }
        } else {
            xlink_send(ch, buf, sz);
        }
    }
    if (zc && zc_pending > 0)
        xlink_zc_poll(ch);
    double t1 = now_ms();

    printf(" %.2f ms (%.1f MB/s)\n", t1 - t0, calc_mbps(n, sz, t1 - t0));
    CHECK(t1 - t0 > 0, "completed");

    free(buf);
    return 0;
}

static int tcp_bench(int port, int n, int sz, int zc) {
    printf("  TCP %s %d×%dB:", zc ? "send_zc" : "send   ", n, sz);

    pid_t pid = fork();
    if (pid < 0) return 1;
    if (pid == 0) tcp_echo(port);
    usleep(150000);

    char addr[64];
    snprintf(addr, sizeof(addr), "127.0.0.1:%d", port);
    xlink_channel_t *ch = xlink_open(XLINK_TCP, addr, &(xlink_opt_t){0});
    if (!ch) {
        kill(pid, SIGKILL); waitpid(pid, NULL, 0);
        CHECK(0, "open"); return 1;
    }

    int rc = tcp_send_bench(ch, n, sz, zc);
    xlink_close(ch);
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    return rc;
}

/* ──── Main ────────────────────────────────────── */

int main(void) {
    printf("=== Zero-Copy Performance Benchmarks ===\n");
    printf("SHM: %d iters | TCP: %d iters\n", SHM_N_ITER, TCP_N_ITER);

    printf("\n--- SHM Small (%dB) ---", MSG_SMALL);
    fflush(stdout);
    shm_bench(SHM_N_ITER, MSG_SMALL, 0);
    shm_bench(SHM_N_ITER, MSG_SMALL, 1);

    printf("\n--- SHM Large (%dB) ---", MSG_LARGE);
    fflush(stdout);
    shm_bench(SHM_N_ITER, MSG_LARGE, 0);
    shm_bench(SHM_N_ITER, MSG_LARGE, 1);

    printf("\n--- TCP Small (%dB) ---", MSG_SMALL);
    fflush(stdout);
    tcp_bench(BENCH_PORT,     TCP_N_ITER, MSG_SMALL, 0);
    tcp_bench(BENCH_PORT + 1, TCP_N_ITER, MSG_SMALL, 1);

    printf("\n--- TCP Large (%dB) ---", MSG_LARGE);
    fflush(stdout);
    tcp_bench(BENCH_PORT + 2, TCP_N_ITER, MSG_LARGE, 0);
    tcp_bench(BENCH_PORT + 3, TCP_N_ITER, MSG_LARGE, 1);

    printf("\n=== Results: %d checks, %d failures ===\n", checks, fails);
    return (fails > 0) ? 1 : 0;
}
