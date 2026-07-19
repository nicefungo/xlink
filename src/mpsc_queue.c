/*
 * mpsc_queue.c — Lock-free MPSC queue implementation
 */

#include "mpsc_queue.h"
#include <stdlib.h>

int xlink_mpsc_init(xlink_mpsc_queue_t *q, int n_producers, size_t slot_capacity)
{
    int i;

    if (n_producers <= 0)
        return -1;

    q->slots = (xlink_spsc_queue_t **)calloc((size_t)n_producers,
                                              sizeof(xlink_spsc_queue_t *));
    if (!q->slots)
        return -1;

    for (i = 0; i < n_producers; i++) {
        q->slots[i] = (xlink_spsc_queue_t *)calloc(1, sizeof(xlink_spsc_queue_t));
        if (!q->slots[i])
            goto fail;

        if (xlink_spsc_init(q->slots[i], slot_capacity) != 0)
            goto fail;
    }

    q->n_producers = n_producers;
    q->rd_idx = 0;
    return 0;

fail:
    while (--i >= 0) {
        xlink_spsc_destroy(q->slots[i]);
        free(q->slots[i]);
    }
    free(q->slots);
    q->slots = NULL;
    return -1;
}

void xlink_mpsc_destroy(xlink_mpsc_queue_t *q)
{
    int i;

    if (!q->slots)
        return;

    for (i = 0; i < q->n_producers; i++) {
        if (q->slots[i]) {
            xlink_spsc_destroy(q->slots[i]);
            free(q->slots[i]);
        }
    }
    free(q->slots);
    q->slots = NULL;
    q->n_producers = 0;
    q->rd_idx = 0;
}

int xlink_mpsc_enqueue(xlink_mpsc_queue_t *q, int pid, void *item)
{
    if (pid < 0 || pid >= q->n_producers || !q->slots[pid])
        return -1;

    return xlink_spsc_enqueue(q->slots[pid], item);
}

int xlink_mpsc_dequeue(xlink_mpsc_queue_t *q, void **out)
{
    int i, start;

    if (!q->n_producers)
        return -1;

    /* Round-robin: try each slot starting from rd_idx */
    start = q->rd_idx;
    for (i = 0; i < q->n_producers; i++) {
        int slot = (start + i) % q->n_producers;
        if (xlink_spsc_dequeue(q->slots[slot], out) == 0) {
            q->rd_idx = (slot + 1) % q->n_producers;  /* advance past drained slot */
            return 0;
        }
    }

    return -1;  /* all empty */
}

size_t xlink_mpsc_count(xlink_mpsc_queue_t *q)
{
    size_t total = 0;
    int i;

    for (i = 0; i < q->n_producers; i++)
        total += xlink_spsc_count(q->slots[i]);

    return total;
}

int xlink_mpsc_empty(xlink_mpsc_queue_t *q)
{
    int i;

    for (i = 0; i < q->n_producers; i++) {
        if (!xlink_spsc_empty(q->slots[i]))
            return 0;
    }
    return 1;
}
