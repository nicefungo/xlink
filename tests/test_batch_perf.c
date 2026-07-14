/*
 * test_batch_perf.c — xlink_send_batch() TCP benchmark
 *
 * Compares throughput of individual xlink_send() vs batched xlink_send_batch()
 * over TCP (loopback).  Uses a simple echo server pattern: client sends N
 * messages, server counts received bytes, client measures wall-clock time.
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

#define TEST_PORT    20100
#define PAYLOAD_SIZE 256
#define N_MESSAGES   1000
#define BATCH_SIZE   50

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

/* ─── Echo server (raw sockets, no xlink) ──────────────── */
static int run_echo_server(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("server socket"); return -1; }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port = htons(TEST_PORT);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("server bind"); close(fd); return -1;
    }
    if (listen(fd, 1) < 0) { perror("server listen"); close(fd); return -1; }

    /* Signal parent we're ready */
    printf("sr "); fflush(stdout);

    struct sockaddr_in cli_addr = {0};
    socklen_t cli_len = sizeof(cli_addr);
    int cfd = accept(fd, (struct sockaddr *)&cli_addr, &cli_len);
    if (cfd < 0) { perror("accept"); close(fd); return -1; }

    /* Read and discard all data (length-prefixed frames) */
    char buf[8192];
    for (;;) {
        ssize_t n = read(cfd, buf, sizeof(buf));
        if (n <= 0) break;
    }
    close(cfd);
    close(fd);
    return 0;
}

/* ─── Benchmark: individual xlink_send ─────────────────── */
static void bench_individual(void) {
    char addr[64];
    snprintf(addr, sizeof(addr), "127.0.0.1:%d", TEST_PORT);

    xlink_channel_t *ch = xlink_open(XLINK_TCP, addr, NULL);
    if (!ch) { printf("FAIL open\n"); return; }

    char payload[PAYLOAD_SIZE];
    memset(payload, 'x', PAYLOAD_SIZE);

    double t0 = now_ms();
    for (int i = 0; i < N_MESSAGES; i++) {
        if (xlink_send(ch, payload, PAYLOAD_SIZE) != 0) {
            printf("FAIL send %d\n", i); xlink_close(ch); return;
        }
    }
    double t1 = now_ms();

    printf("individual: %.1f ms (%d sends, %.1f KB/s)\n",
           t1 - t0, N_MESSAGES,
           (N_MESSAGES * PAYLOAD_SIZE / 1024.0) / ((t1 - t0) / 1000.0));

    xlink_close(ch);
}

/* ─── Benchmark: batched xlink_send_batch ───────────────── */
static void bench_batched(void) {
    char addr[64];
    snprintf(addr, sizeof(addr), "127.0.0.1:%d", TEST_PORT);

    xlink_channel_t *ch = xlink_open(XLINK_TCP, addr, NULL);
    if (!ch) { printf("FAIL open\n"); return; }

    char payload[PAYLOAD_SIZE];
    memset(payload, 'x', PAYLOAD_SIZE);

    xlink_msg_t msgs[BATCH_SIZE];
    for (int i = 0; i < BATCH_SIZE; i++) {
        msgs[i].data = payload;
        msgs[i].len  = PAYLOAD_SIZE;
    }

    double t0 = now_ms();
    int remaining = N_MESSAGES;
    while (remaining > 0) {
        int n = (remaining < BATCH_SIZE) ? remaining : BATCH_SIZE;
        int rc = xlink_send_batch(ch, msgs, n);
        if (rc != n) {
            printf("FAIL batch returned %d\n", rc); xlink_close(ch); return;
        }
        remaining -= n;
    }
    double t1 = now_ms();

    printf("batched:    %.1f ms (%d sends, batch=%d, %.1f KB/s)\n",
           t1 - t0, N_MESSAGES, BATCH_SIZE,
           (N_MESSAGES * PAYLOAD_SIZE / 1024.0) / ((t1 - t0) / 1000.0));

    xlink_close(ch);
}

int main(void) {
    printf("=== xlink TCP batch performance benchmark ===\n");
    printf("messages: %d × %d bytes, batch size: %d\n\n",
           N_MESSAGES, PAYLOAD_SIZE, BATCH_SIZE);

    /* ── Individual send benchmark ──────── */
    {
        pid_t pid = fork();
        if (pid == 0) { _exit(run_echo_server()); }

        /* Wait for server ready signal */
        char c;
        if (read(STDIN_FILENO, &c, 1) <= 0) { /* parent reads from pipe */ }

        /* Actually, need a sync mechanism. The server prints to stdout.
         * Use a pipe-based sync: wait a short period for server to bind. */
        usleep(100000); /* 100ms should be enough */

        bench_individual();
        waitpid(pid, NULL, 0);
    }

    /* ── Batched send benchmark ────────── */
    {
        pid_t pid = fork();
        if (pid == 0) { _exit(run_echo_server()); }
        usleep(100000);

        bench_batched();
        waitpid(pid, NULL, 0);
    }

    printf("\n=== Benchmarks complete ===\n");
    return 0;
}
