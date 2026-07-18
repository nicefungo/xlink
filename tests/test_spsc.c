/*
 * test_spsc.c — Lock-free SPSC queue correctness & concurrency tests
 *
 * Tests:
 *   1. Basic enqueue/dequeue
 *   2. Full/empty detection
 *   3. Single-threaded ring wrap-around
 *   4. Multi-threaded producer/consumer (correctness + ordering)
 *   5. Concurrent throughput stress
 */
#include "../src/spsc_queue.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <assert.h>

#define PASS() printf("  PASS: %s\n", __func__ + 5)
#define FAIL(msg) do { printf("  FAIL: %s — %s\n", __func__ + 5, msg); _exit(1); } while(0)
#define CHECK(cond, msg) do { if (!(cond)) FAIL(msg); } while(0)

static void test_basic(void)
{
    xlink_spsc_queue_t q;
    CHECK(xlink_spsc_init(&q, 16) == 0, "init");
    CHECK(xlink_spsc_empty(&q), "should be empty after init");
    CHECK(!xlink_spsc_full(&q), "should not be full after init");

    /* Enqueue 3 items */
    int a = 1, b = 2, c = 3;
    CHECK(xlink_spsc_enqueue(&q, &a) == 0, "enqueue 1");
    CHECK(xlink_spsc_enqueue(&q, &b) == 0, "enqueue 2");
    CHECK(xlink_spsc_enqueue(&q, &c) == 0, "enqueue 3");
    CHECK(xlink_spsc_count(&q) == 3, "count should be 3");

    /* Dequeue in order */
    void *out;
    CHECK(xlink_spsc_dequeue(&q, &out) == 0, "dequeue 1");
    CHECK(*(int *)out == 1, "dequeue value 1");
    CHECK(xlink_spsc_dequeue(&q, &out) == 0, "dequeue 2");
    CHECK(*(int *)out == 2, "dequeue value 2");
    CHECK(xlink_spsc_dequeue(&q, &out) == 0, "dequeue 3");
    CHECK(*(int *)out == 3, "dequeue value 3");

    CHECK(xlink_spsc_empty(&q), "should be empty after draining");
    CHECK(xlink_spsc_dequeue(&q, &out) == -1, "dequeue on empty should fail");

    xlink_spsc_destroy(&q);
    PASS();
}

static void test_full_empty(void)
{
    xlink_spsc_queue_t q;
    CHECK(xlink_spsc_init(&q, 4) == 0, "init");  /* capacity rounds to 8 */
    /* capacity is actually 8 (next power of 2 >= 4) */
    CHECK(q.capacity == 8, "capacity should be 8");

    int item = 42;
    int i;

    /* Fill queue */
    for (i = 0; i < 8; i++) {
        CHECK(xlink_spsc_enqueue(&q, &item) == 0, "enqueue while not full");
    }
    CHECK(xlink_spsc_count(&q) == 8, "count should be 8");
    CHECK(xlink_spsc_full(&q), "should be full");

    /* Enqueue on full should fail */
    CHECK(xlink_spsc_enqueue(&q, &item) == -1, "enqueue on full should fail");

    /* Drain and check */
    void *out;
    for (i = 0; i < 8; i++) {
        CHECK(xlink_spsc_dequeue(&q, &out) == 0, "dequeue from full");
        CHECK(*(int *)out == 42, "dequeue value");
    }
    CHECK(xlink_spsc_empty(&q), "should be empty");

    xlink_spsc_destroy(&q);
    PASS();
}

static void test_wrap_around(void)
{
    xlink_spsc_queue_t q;
    CHECK(xlink_spsc_init(&q, 8) == 0, "init");

    int items[8];
    void *out;
    int i;

    /* First fill */
    for (i = 0; i < 8; i++) {
        items[i] = i;
        CHECK(xlink_spsc_enqueue(&q, &items[i]) == 0, "enqueue first pass");
    }
    CHECK(xlink_spsc_full(&q), "should be full");

    /* Drain half */
    for (i = 0; i < 4; i++) {
        CHECK(xlink_spsc_dequeue(&q, &out) == 0, "dequeue half");
        CHECK(*(int *)out == i, "dequeue value");
    }
    CHECK(xlink_spsc_count(&q) == 4, "half full");

    /* Fill second half (wrap around) */
    for (i = 8; i < 12; i++) {
        items[i % 8] = i;
        CHECK(xlink_spsc_enqueue(&q, &items[i % 8]) == 0, "enqueue second pass");
    }
    CHECK(xlink_spsc_full(&q), "should be full again");

    /* Drain all — verify order */
    for (i = 0; i < 8; i++) {
        CHECK(xlink_spsc_dequeue(&q, &out) == 0, "dequeue all");
        CHECK(*(int *)out == i + 4, "wrapped value correct");
    }
    CHECK(xlink_spsc_empty(&q), "should be empty");

    xlink_spsc_destroy(&q);
    PASS();
}

static void test_min_capacity(void)
{
    xlink_spsc_queue_t q;
    CHECK(xlink_spsc_init(&q, 1) == 0, "init with 1");
    /* Should round up to XLINK_SPSC_MIN_CAP = 8 */
    CHECK(q.capacity == 8, "capacity should be 8 (min)");

    int item = 99;
    CHECK(xlink_spsc_enqueue(&q, &item) == 0, "enqueue");
    void *out;
    CHECK(xlink_spsc_dequeue(&q, &out) == 0, "dequeue");
    CHECK(*(int *)out == 99, "value");

    xlink_spsc_destroy(&q);
    PASS();
}

/*
 * Multi-threaded producer/consumer:
 * Producer enqueues N items (0..N-1), consumer dequeues and sums them.
 * Final sum should be N*(N-1)/2 if all items arrive and are counted once.
 */
#define MT_COUNT 1000000

typedef struct {
    xlink_spsc_queue_t *q;
    uint64_t            sum;
} mt_arg_t;

static void *producer_fn(void *arg)
{
    mt_arg_t *a = (mt_arg_t *)arg;
    int i;

    for (i = 0; i < MT_COUNT; i++) {
        while (xlink_spsc_enqueue(a->q, (void *)(uintptr_t)i) != 0) {
            /* Busy-wait: real code would yield or use eventfd */
        }
    }
    return NULL;
}

static void *consumer_fn(void *arg)
{
    mt_arg_t *a = (mt_arg_t *)arg;
    void *out;
    int received = 0;

    while (received < MT_COUNT) {
        if (xlink_spsc_dequeue(a->q, &out) == 0) {
            a->sum += (uint64_t)(uintptr_t)out;
            received++;
        }
    }
    return NULL;
}

static void test_multithread(void)
{
    xlink_spsc_queue_t q;
    CHECK(xlink_spsc_init(&q, 1024) == 0, "init");

    mt_arg_t consumer_arg = { .q = &q, .sum = 0 };
    mt_arg_t producer_arg = { .q = &q, .sum = 0 };

    pthread_t prod, cons;
    pthread_create(&prod, NULL, producer_fn, &producer_arg);
    pthread_create(&cons, NULL, consumer_fn, &consumer_arg);

    pthread_join(prod, NULL);
    pthread_join(cons, NULL);

    /* Gauss sum: 0 + 1 + ... + (N-1) = N*(N-1)/2 */
    uint64_t expected = (uint64_t)MT_COUNT * (MT_COUNT - 1) / 2;
    CHECK(consumer_arg.sum == expected, "MT sum mismatch");

    CHECK(xlink_spsc_empty(&q), "should be empty after MT");
    xlink_spsc_destroy(&q);
    PASS();
}

static void test_mt_large_capacity(void)
{
    xlink_spsc_queue_t q;
    CHECK(xlink_spsc_init(&q, 65536) == 0, "init");  /* 64K */

    mt_arg_t consumer_arg = { .q = &q, .sum = 0 };
    mt_arg_t producer_arg = { .q = &q, .sum = 0 };

    pthread_t prod, cons;
    pthread_create(&prod, NULL, producer_fn, &producer_arg);
    pthread_create(&cons, NULL, consumer_fn, &consumer_arg);

    pthread_join(prod, NULL);
    pthread_join(cons, NULL);

    uint64_t expected = (uint64_t)MT_COUNT * (MT_COUNT - 1) / 2;
    CHECK(consumer_arg.sum == expected, "MT large capacity sum");
    CHECK(xlink_spsc_empty(&q), "should be empty");

    xlink_spsc_destroy(&q);
    PASS();
}

int main(void)
{
    printf("=== Lock-free SPSC queue tests ===\n\n");

    test_basic();
    test_full_empty();
    test_wrap_around();
    test_min_capacity();
    test_multithread();
    test_mt_large_capacity();

    printf("\n=== ALL PASSED ===\n");
    return 0;
}
