/*
 * test_mpsc.c — Lock-free MPSC queue tests
 *
 * Tests multi-producer single-consumer behavior:
 *   1. Basic single-producer (should work same as SPSC)
 *   2. Two producers, interleaved enqueue, consumer sees all
 *   3. Multi-threaded: 4 producers concurrently enqueue, 1 consumer dequeues all
 *   4. Full slot detection (one producer fills, others still can enqueue)
 *   5. Round-robin fairness
 *   6. Error paths
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include "../src/mpsc_queue.h"

#define NMESSAGES 50000
#define NPRODUCERS 4
#define SLOT_CAP 131072  /* large enough to hold all messages without blocking */

static int npass, nfail;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); npass++; } \
    else { printf("  FAIL: %s\n", msg); nfail++; } \
} while (0)

/* ── Test 1: Single producer (degenerate case) ── */
static void test_single_producer(void)
{
    xlink_mpsc_queue_t q;
    void *items[5];
    void *out;
    int i;

    printf("\n--- Single producer ---\n");

    CHECK(xlink_mpsc_init(&q, 1, 8) == 0, "init 1-producer");

    for (i = 0; i < 3; i++) {
        items[i] = (void *)(intptr_t)(i + 1);
        CHECK(xlink_mpsc_enqueue(&q, 0, items[i]) == 0, "enqueue single");
    }

    CHECK(xlink_mpsc_count(&q) == 3, "count == 3");

    for (i = 0; i < 3; i++) {
        CHECK(xlink_mpsc_dequeue(&q, &out) == 0, "dequeue single");
        CHECK(out == items[i], "dequeue order correct");
    }

    CHECK(xlink_mpsc_empty(&q), "empty after drain");
    CHECK(xlink_mpsc_dequeue(&q, &out) == -1, "dequeue empty returns -1");

    xlink_mpsc_destroy(&q);
}

/* ── Test 2: Two producers interleaved ── */
static void test_two_producers(void)
{
    xlink_mpsc_queue_t q;
    void *a = (void *)1, *b = (void *)2, *c = (void *)3, *d = (void *)4;
    void *out;
    int received[4] = {0};

    printf("\n--- Two producers interleaved ---\n");

    CHECK(xlink_mpsc_init(&q, 2, 16) == 0, "init 2-producer");

    CHECK(xlink_mpsc_enqueue(&q, 0, a) == 0, "p0 enqueue a");
    CHECK(xlink_mpsc_enqueue(&q, 1, b) == 0, "p1 enqueue b");
    CHECK(xlink_mpsc_enqueue(&q, 0, c) == 0, "p0 enqueue c");
    CHECK(xlink_mpsc_enqueue(&q, 1, d) == 0, "p1 enqueue d");

    CHECK(xlink_mpsc_count(&q) == 4, "count == 4");

    while (xlink_mpsc_dequeue(&q, &out) == 0) {
        received[(intptr_t)out - 1] = 1;
    }

    CHECK(received[0] && received[1] && received[2] && received[3],
          "all 4 items received (order may vary by round-robin)");
    CHECK(xlink_mpsc_empty(&q), "empty after drain");

    xlink_mpsc_destroy(&q);
}

/* ── Test 3: Full slot isolation ── */
static void test_full_slot_isolation(void)
{
    xlink_mpsc_queue_t q;
    int i;

    printf("\n--- Full slot isolation ---\n");

    /* Create 2-producer MPSC with tiny slot capacity (8) */
    CHECK(xlink_mpsc_init(&q, 2, 8) == 0, "init 2-producer small");

    /* Fill producer 0's slot */
    for (i = 0; i < 7; i++)
        CHECK(xlink_mpsc_enqueue(&q, 0, (void *)(intptr_t)i) == 0, "fill p0");

    /* Producer 0 should be full now (8 slots, 7 filled = 1 slot margin for SPSC) */
    /* Actually SPSC uses head-tail >= capacity as full, so with 8 slots, 7 is full
       (capacity uses 1-slot gap for full detection) */
    /* Let's fill to exact capacity-1 */
    /* OK actually SPSC uses `(head - tail) >= capacity` — so with cap=8, 8 items fit
       but the full check means at 8 items it says full. So 7 is not full. Let's fill more. */
    /* The SPSC full check is (head - tail) >= capacity. Head goes up to 7, tail=0 => 7 >= 8 is false. 
       Head=8, tail=0 => 8 >= 8 is true => full. So 8 items fit. */
    /* Actually we only need to show p1 can still enqueue when p0 is full */
    /* Let's just fill p0 completely */

    /* Producer 1 should still be able to enqueue */
    for (i = 0; i < 8; i++) {
        int rc = xlink_mpsc_enqueue(&q, 0, (void *)(intptr_t)(100 + i));
        if (rc == -1) break; /* p0 full */
    }

    /* Now try p1 */
    CHECK(xlink_mpsc_enqueue(&q, 1, (void *)42) == 0,
          "p1 can enqueue even when p0 is full");

    CHECK(xlink_mpsc_count(&q) >= 1, "still has items (from p1)");

    xlink_mpsc_destroy(&q);
}

/* ── Test 4: Multi-threaded N producers → 1 consumer ── */
typedef struct {
    xlink_mpsc_queue_t *q;
    int pid;
    int nmsg;
} prod_arg_t;

static void *producer_thread(void *arg)
{
    prod_arg_t *a = (prod_arg_t *)arg;
    int i;

    for (i = 0; i < a->nmsg; i++) {
        void *item = (void *)(intptr_t)(a->pid * 1000000 + i);
        while (xlink_mpsc_enqueue(a->q, a->pid, item) == -1)
            ; /* busy-wait until slot has room */
    }
    return NULL;
}

static void test_multithread(void)
{
    xlink_mpsc_queue_t q;
    pthread_t producers[NPRODUCERS];
    prod_arg_t args[NPRODUCERS];
    int received[NPRODUCERS][NMESSAGES];
    int pid_counts[NPRODUCERS];
    void *out;
    int expected, got;
    int i;

    printf("\n--- Multi-threaded: %d producers × %d msgs ---\n",
           NPRODUCERS, NMESSAGES);

    memset(received, 0, sizeof(received));
    memset(pid_counts, 0, sizeof(pid_counts));

    CHECK(xlink_mpsc_init(&q, NPRODUCERS, SLOT_CAP) == 0,
          "init MT MPSC");

    for (i = 0; i < NPRODUCERS; i++) {
        args[i].q = &q;
        args[i].pid = i;
        args[i].nmsg = NMESSAGES;
        pthread_create(&producers[i], NULL, producer_thread, &args[i]);
    }

    for (i = 0; i < NPRODUCERS; i++)
        pthread_join(producers[i], NULL);

    /* Consumer: dequeue everything */
    while (xlink_mpsc_dequeue(&q, &out) == 0) {
        int pid = (int)((intptr_t)out / 1000000);
        int seq = (int)((intptr_t)out % 1000000);

        if (pid >= 0 && pid < NPRODUCERS && seq >= 0 && seq < NMESSAGES) {
            received[pid][seq] = 1;
            pid_counts[pid]++;
        }
    }

    CHECK(xlink_mpsc_empty(&q), "queue empty after drain");

    /* Verify all messages from all producers */
    expected = NPRODUCERS * NMESSAGES;
    got = 0;
    for (i = 0; i < NPRODUCERS; i++)
        got += pid_counts[i];

    CHECK(got == expected, "received all messages");

    /* Verify no duplicates and no missing */
    for (i = 0; i < NPRODUCERS; i++) {
        int j;
        for (j = 0; j < NMESSAGES; j++) {
            if (!received[i][j]) {
                printf("  FAIL: producer %d missing msg %d\n", i, j);
                nfail++;
                goto done_verify;
            }
        }
    }
    CHECK(1, "all messages accounted (no gaps, no duplicates)");

done_verify:
    xlink_mpsc_destroy(&q);
}

/* ── Test 5: Round-robin fairness ── */
static void test_round_robin(void)
{
    xlink_mpsc_queue_t q;
    int i;

    printf("\n--- Round-robin fairness ---\n");

    CHECK(xlink_mpsc_init(&q, 4, 32) == 0, "init for RR test");

    /* Enqueue 4 items from each of 4 producers */
    for (i = 0; i < 4; i++) {
        xlink_mpsc_enqueue(&q, 0, (void *)(intptr_t)(0 * 10 + i));
        xlink_mpsc_enqueue(&q, 1, (void *)(intptr_t)(1 * 10 + i));
        xlink_mpsc_enqueue(&q, 2, (void *)(intptr_t)(2 * 10 + i));
        xlink_mpsc_enqueue(&q, 3, (void *)(intptr_t)(3 * 10 + i));
    }

    CHECK(xlink_mpsc_count(&q) == 16, "16 items total");

    /* Drain and collect order */
    int order[16];
    int n = 0;
    void *out;
    while (xlink_mpsc_dequeue(&q, &out) == 0)
        order[n++] = (int)(intptr_t)out;

    CHECK(n == 16, "drained 16 items");

    /* Verify round-robin: p0,p1,p2,p3,p0,p1,... pattern */
    int rr_ok = 1;
    for (i = 0; i < 16; i++) {
        int expected_pid = i % 4;
        int actual_pid = order[i] / 10;
        if (actual_pid != expected_pid)
            rr_ok = 0;
    }
    CHECK(rr_ok, "round-robin order preserved");

    xlink_mpsc_destroy(&q);
}

/* ── Test 6: Error paths ── */
static void test_errors(void)
{
    xlink_mpsc_queue_t q;
    void *out;

    printf("\n--- Error paths ---\n");

    /* Invalid n_producers */
    CHECK(xlink_mpsc_init(&q, 0, 8) == -1, "init with 0 producers fails");
    CHECK(xlink_mpsc_init(&q, -1, 8) == -1, "init with -1 producers fails");

    /* Valid init */
    CHECK(xlink_mpsc_init(&q, 2, 8) == 0, "init 2-producer for error tests");

    /* Invalid pid enqueue */
    CHECK(xlink_mpsc_enqueue(&q, -1, (void *)1) == -1, "enqueue pid=-1 fails");
    CHECK(xlink_mpsc_enqueue(&q, 2, (void *)1) == -1, "enqueue pid=2 fails (only 0,1 valid)");

    /* Empty dequeue */
    CHECK(xlink_mpsc_dequeue(&q, &out) == -1, "dequeue empty fails");
    CHECK(xlink_mpsc_empty(&q), "empty() returns 1");
    CHECK(xlink_mpsc_count(&q) == 0, "count() returns 0 on empty");

    /* Double destroy should be safe */
    xlink_mpsc_destroy(&q);
    xlink_mpsc_destroy(&q);  /* should not crash */
    CHECK(1, "double destroy no crash");
}

int main(void)
{
    printf("=== Lock-free MPSC queue tests ===\n");
    printf("(reuses per-producer SPSC slots)\n");

    test_single_producer();
    test_two_producers();
    test_full_slot_isolation();
    test_multithread();
    test_round_robin();
    test_errors();

    printf("\n=== RESULTS: %d/%d PASS ===\n", npass, npass + nfail);
    return nfail > 0 ? 1 : 0;
}
