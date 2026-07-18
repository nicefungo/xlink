/*
 * spsc_queue.h — Lock-free Single Producer Single Consumer ring buffer
 *
 * Based on Lamport's classic algorithm. Uses C11 atomics with
 * acquire/release ordering. Zero syscalls on hot path.
 *
 * Memory ordering analysis:
 *   - Producer: store buffer[] (relaxed), then atomic_store(head, release)
 *     → release ensures buffer[] writes are visible before head advances
 *   - Consumer: atomic_load(head, acquire), then load buffer[] (relaxed)
 *     → acquire ensures buffer[] reads see the write that updated head
 *
 * Cache-line padded to prevent false sharing between producer/consumer cores.
 */

#ifndef XLINK_SPSC_QUEUE_H
#define XLINK_SPSC_QUEUE_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#define XLINK_CACHE_LINE 64
#define XLINK_SPSC_MIN_CAP 8

typedef struct xlink_spsc_queue {
    void               **buffer;      /* ring buffer of void* slots */
    size_t               mask;        /* capacity - 1 (capacity is power of 2) */
    size_t               capacity;

    /* Producer owns head, consumer owns tail — no contention */
    _Atomic size_t       head;        /* next write index */
    _Atomic size_t       tail;        /* next read index */

    /* Padding to prevent false sharing */
    char                 _pad[XLINK_CACHE_LINE - 3 * sizeof(size_t) - 2 * sizeof(void *)];
} xlink_spsc_queue_t;

/*
 * Initialize a pre-allocated queue. capacity is rounded up to next power of 2.
 * Returns 0 on success, -1 on invalid capacity.
 */
int xlink_spsc_init(xlink_spsc_queue_t *q, size_t capacity);

/*
 * Destroy the queue. Does NOT free items stored in the queue —
 * caller must drain before destroying if items need cleanup.
 */
void xlink_spsc_destroy(xlink_spsc_queue_t *q);

/*
 * Enqueue an item. Returns 0 on success, -1 if queue is full.
 * Caller retains ownership of *item until dequeued.
 */
static inline int xlink_spsc_enqueue(xlink_spsc_queue_t *q, void *item)
{
    size_t head = atomic_load_explicit(&q->head, memory_order_relaxed);
    size_t tail = atomic_load_explicit(&q->tail, memory_order_acquire);

    if ((head - tail) >= q->capacity)
        return -1;  /* full */

    q->buffer[head & q->mask] = item;

    /* Release: ensure buffer[] write is visible before head advances */
    atomic_store_explicit(&q->head, head + 1, memory_order_release);
    return 0;
}

/*
 * Dequeue an item. Returns 0 on success (item stored in *out),
 * -1 if queue is empty.
 */
static inline int xlink_spsc_dequeue(xlink_spsc_queue_t *q, void **out)
{
    size_t tail = atomic_load_explicit(&q->tail, memory_order_relaxed);
    size_t head = atomic_load_explicit(&q->head, memory_order_acquire);

    if (tail == head)
        return -1;  /* empty */

    *out = q->buffer[tail & q->mask];

    /* Release: ensure dequeue completes before tail advances */
    atomic_store_explicit(&q->tail, tail + 1, memory_order_release);
    return 0;
}

/*
 * Return number of items currently in the queue (approximate snapshot).
 */
static inline size_t xlink_spsc_count(xlink_spsc_queue_t *q)
{
    size_t head = atomic_load_explicit(&q->head, memory_order_acquire);
    size_t tail = atomic_load_explicit(&q->tail, memory_order_acquire);
    return head - tail;
}

/*
 * Return 1 if queue is full, 0 otherwise.
 */
static inline int xlink_spsc_full(xlink_spsc_queue_t *q)
{
    size_t head = atomic_load_explicit(&q->head, memory_order_relaxed);
    size_t tail = atomic_load_explicit(&q->tail, memory_order_acquire);
    return (head - tail) >= q->capacity;
}

/*
 * Return 1 if queue is empty, 0 otherwise.
 */
static inline int xlink_spsc_empty(xlink_spsc_queue_t *q)
{
    size_t tail = atomic_load_explicit(&q->tail, memory_order_relaxed);
    size_t head = atomic_load_explicit(&q->head, memory_order_acquire);
    return tail == head;
}

#endif /* XLINK_SPSC_QUEUE_H */
