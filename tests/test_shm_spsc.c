/*
 * test_shm_spsc.c — Lock-free SPSC/MPSC queue + xlink SHM integration
 *
 * Tests:
 *   1. SPSC queue public API (via include/spsc_queue.h)
 *   2. MPSC queue public API (via include/mpsc_queue.h)  
 *   3. SPSC producer → xlink SHM send → recv (single process pipe)
 *   4. MPSC multi-thread producers → xlink SHM send → recv
 */

#include "spsc_queue.h"
#include "mpsc_queue.h"
#include "xlink.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <assert.h>
#include <sys/wait.h>

#define PASS() printf("  PASS: %s\n", __func__ + 5)
#define FAIL(msg) do { printf("  FAIL: %s — %s\n", __func__ + 5, msg); _exit(1); } while(0)
#define CHECK(cond, msg) do { if (!(cond)) FAIL(msg); } while(0)

/* ─── Test 1: SPSC public API via include/ ─── */

static void test_spsc_public_api(void)
{
    xlink_spsc_queue_t q;
    CHECK(xlink_spsc_init(&q, 32) == 0, "init");

    int total = 100;
    int enq = 0, deq = 0;
    while (deq < total) {
        /* Enqueue when possible */
        while (enq < total) {
            int *p = malloc(sizeof(int));
            *p = enq;
            if (xlink_spsc_enqueue(&q, p) == 0) {
                enq++;
            } else {
                free(p);
                break;  /* queue full — drain first */
            }
        }
        /* Drain some */
        void *out;
        while (xlink_spsc_dequeue(&q, &out) == 0) {
            CHECK(*(int *)out == deq, "value seq");
            free(out);
            deq++;
        }
    }
    CHECK(deq == total, "all drained");

    CHECK(xlink_spsc_empty(&q), "empty after drain");
    xlink_spsc_destroy(&q);
    PASS();
}

/* ─── Test 2: MPSC public API via include/ ─── */

static void test_mpsc_public_api(void)
{
    xlink_mpsc_queue_t mq;
    CHECK(xlink_mpsc_init(&mq, 3, 64) == 0, "init 3-producer");

    /* Producer 0 enqueues 10 items */
    for (int i = 0; i < 10; i++) {
        CHECK(xlink_mpsc_enqueue(&mq, 0, (void *)(uintptr_t)(100 + i)) == 0,
              "p0 enqueue");
    }
    /* Producer 1 enqueues 5 */
    for (int i = 0; i < 5; i++) {
        CHECK(xlink_mpsc_enqueue(&mq, 1, (void *)(uintptr_t)(200 + i)) == 0,
              "p1 enqueue");
    }
    /* Producer 2 enqueues 3 */
    for (int i = 0; i < 3; i++) {
        CHECK(xlink_mpsc_enqueue(&mq, 2, (void *)(uintptr_t)(300 + i)) == 0,
              "p2 enqueue");
    }

    CHECK(xlink_mpsc_count(&mq) == 18, "count 18");

    /* Drain — round-robin order preserved */
    int drained = 0;
    void *item;
    while (xlink_mpsc_dequeue(&mq, &item) == 0) {
        drained++;
    }
    CHECK(drained == 18, "drain 18");
    CHECK(xlink_mpsc_empty(&mq), "empty");

    xlink_mpsc_destroy(&mq);
    PASS();
}

/* ─── Test 3: SPSC → xlink SHM (fork isolation) ─── */

static int xrecv_timed(xlink_channel_t *ch, void *buf, size_t *len, int timeout_ms) {
    for (int i = 0; i < timeout_ms / 10; i++) {
        int rc = xlink_recv(ch, buf, len);
        if (rc == 0) return 0;
        usleep(10000);
    }
    return -1;
}

static void test_spsc_shm_pipeline(void)
{
    const char *name = "ts3_spsc";
    pid_t pid = fork();
    CHECK(pid >= 0, "fork for shm pipeline");

    if (pid == 0) {
        /* Child: receiver */
        xlink_channel_t *rx = NULL;
        for (int i = 0; i < 100 && !rx; i++) {
            usleep(50000);
            rx = xlink_open(XLINK_SHM, name, NULL);
        }
        CHECK(rx != NULL, "child open");

        int got = 0;
        for (int i = 0; i < 10; i++) {
            char buf[8];
            size_t len = sizeof(buf);
            int rc = xrecv_timed(rx, buf, &len, 5000);
            CHECK(rc == 0, "child recv");
            CHECK(len == 7, "child len");
            CHECK(memcmp(buf, "msg-", 4) == 0, "child prefix");
            got++;
        }
        xlink_close(rx);
        _exit(got == 10 ? 0 : 1);
    }

    /* Parent: create SHM + use SPSC queue to produce messages */
    xlink_opt_t opt = XLINK_OPT_DEFAULT;
    opt.flags = XLINK_CREATE;
    xlink_channel_t *tx = xlink_open(XLINK_SHM, name, &opt);
    CHECK(tx != NULL, "parent open");

    xlink_spsc_queue_t q;
    CHECK(xlink_spsc_init(&q, 16) == 0, "spsc init");

    char items[10][8];
    for (int i = 0; i < 10; i++) {
        snprintf(items[i], sizeof(items[i]), "msg-%02d", i);
        CHECK(xlink_spsc_enqueue(&q, items[i]) == 0, "enqueue");
    }

    int sent = 0;
    void *out;
    while (xlink_spsc_dequeue(&q, &out) == 0) {
        CHECK(xlink_send(tx, (char *)out, 7) == 0, "send");
        sent++;
    }
    CHECK(sent == 10, "all sent");

    xlink_spsc_destroy(&q);
    xlink_close(tx);

    int st;
    waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "child ok");

    {
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "rm -f /dev/shm/%s /tmp/xlink-evt-%s 2>/dev/null", name, name);
        system(cmd);
    }
    PASS();
}

/* ─── Test 4: MPSC multi-threaded → SHM ─── */

#define MPSC_N 200
#define MPSC_PRODS 4

typedef struct {
    xlink_mpsc_queue_t *q;
    int pid;
} mprod_arg_t;

static void *mpsc_prod_fn(void *arg)
{
    mprod_arg_t *a = (mprod_arg_t *)arg;
    int base = a->pid * 10000;
    int per = MPSC_N / MPSC_PRODS;
    for (int i = 0; i < per; i++) {
        int val = base + i;
        while (xlink_mpsc_enqueue(a->q, a->pid, (void *)(uintptr_t)val) != 0);
    }
    return NULL;
}

static void test_mpsc_shm_pipeline(void)
{
    const char *name = "ts4_mpsc";
    pid_t pid = fork();
    CHECK(pid >= 0, "fork mpsc");

    if (pid == 0) {
        xlink_channel_t *rx = NULL;
        for (int i = 0; i < 100 && !rx; i++) {
            usleep(50000);
            rx = xlink_open(XLINK_SHM, name, NULL);
        }
        CHECK(rx != NULL, "child mpsc open");

        int got = 0;
        for (int i = 0; i < MPSC_N; i++) {
            int val;
            size_t len = sizeof(val);
            int rc = xrecv_timed(rx, &val, &len, 5000);
            if (rc == 0) got++;
            else { i--; continue; }
        }
        xlink_close(rx);
        _exit(got >= MPSC_N * 90 / 100 ? 0 : 1);
    }

    /* Parent */
    xlink_opt_t opt = XLINK_OPT_DEFAULT;
    opt.flags = XLINK_CREATE;
    xlink_channel_t *tx = xlink_open(XLINK_SHM, name, &opt);
    CHECK(tx != NULL, "parent mpsc open");

    xlink_mpsc_queue_t mq;
    CHECK(xlink_mpsc_init(&mq, MPSC_PRODS, 128) == 0, "mpsc init");

    pthread_t th[MPSC_PRODS];
    mprod_arg_t args[MPSC_PRODS];
    for (int i = 0; i < MPSC_PRODS; i++) {
        args[i].q = &mq;
        args[i].pid = i;
        pthread_create(&th[i], NULL, mpsc_prod_fn, &args[i]);
    }

    /* Drain MPSC → xlink send */
    int sent = 0;
    void *item;
    int stall = 0;
    while (sent < MPSC_N && stall < 1000) {
        if (xlink_mpsc_dequeue(&mq, &item) == 0) {
            int val = (int)(uintptr_t)item;
            CHECK(xlink_send(tx, &val, sizeof(val)) == 0, "send val");
            sent++;
            stall = 0;
        } else {
            usleep(1000);
            stall++;
        }
    }

    for (int i = 0; i < MPSC_PRODS; i++) pthread_join(th[i], NULL);
    CHECK(sent == MPSC_N, "all mpsc sent");

    xlink_mpsc_destroy(&mq);
    xlink_close(tx);

    int st;
    waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "child mpsc ok");

    {
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "rm -f /dev/shm/%s /tmp/xlink-evt-%s 2>/dev/null", name, name);
        system(cmd);
    }
    PASS();
}

int main(void)
{
    printf("=== SHM + Lock-free SPSC/MPSC Integration ===\n\n");

    test_spsc_public_api();
    test_mpsc_public_api();
    test_spsc_shm_pipeline();
    test_mpsc_shm_pipeline();

    printf("\n=== ALL PASSED ===\n");
    return 0;
}
