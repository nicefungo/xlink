/*
 * mpsc_queue.h — Lock-free Multi-Producer Single-Consumer queue
 *
 * Strategy: per-producer SPSC slots. Each producer writes to its own dedicated
 * SPSC queue (zero contention between producers). The consumer round-robins
 * through all slots, dequeuing from each in turn.
 *
 * This avoids the expensive CAS contention of a single shared queue while
 * keeping the implementation simple (reuses existing SPSC infra).
 *
 * Use case: SHM broadcast/aggregate scenarios, multi-threaded data pipelines.
 */

#ifndef XLINK_MPSC_QUEUE_H
#define XLINK_MPSC_QUEUE_H

#include "spsc_queue.h"
#include <stddef.h>

typedef struct xlink_mpsc_queue {
    xlink_spsc_queue_t **slots;    /* per-producer SPSC queues */
    int                  n_producers;  /* number of producers */
    int                  rd_idx;       /* consumer round-robin cursor */
} xlink_mpsc_queue_t;

/*
 * Initialize MPSC queue with n_producers slots.
 * Each slot's capacity is rounded up to next power of 2 (min 8).
 * Returns 0 on success, -1 on alloc failure.
 */
int xlink_mpsc_init(xlink_mpsc_queue_t *q, int n_producers, size_t slot_capacity);

/*
 * Destroy the MPSC queue and all internal SPSC slots.
 * Does NOT free items stored in the queue — caller must drain first.
 */
void xlink_mpsc_destroy(xlink_mpsc_queue_t *q);

/*
 * Enqueue an item into producer pid's slot (0 <= pid < n_producers).
 * Returns 0 on success, -1 if that producer's slot is full.
 */
int xlink_mpsc_enqueue(xlink_mpsc_queue_t *q, int pid, void *item);

/*
 * Dequeue an item. Consumer round-robins through all producer slots.
 * Returns 0 on success (item stored in *out), -1 if all slots are empty.
 */
int xlink_mpsc_dequeue(xlink_mpsc_queue_t *q, void **out);

/*
 * Return total number of items across all producer slots.
 */
size_t xlink_mpsc_count(xlink_mpsc_queue_t *q);

/*
 * Return 1 if all slots are empty, 0 otherwise.
 */
int xlink_mpsc_empty(xlink_mpsc_queue_t *q);

#endif /* XLINK_MPSC_QUEUE_H */
