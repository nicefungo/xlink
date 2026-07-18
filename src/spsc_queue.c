/*
 * spsc_queue.c — Lock-free SPSC queue implementation (non-inline functions)
 */

#include "spsc_queue.h"
#include <stdlib.h>

int xlink_spsc_init(xlink_spsc_queue_t *q, size_t capacity)
{
    size_t cap;

    if (capacity < XLINK_SPSC_MIN_CAP)
        capacity = XLINK_SPSC_MIN_CAP;

    /* Round up to next power of 2 */
    cap = 1;
    while (cap < capacity)
        cap <<= 1;

    q->buffer = (void **)calloc(cap, sizeof(void *));
    if (!q->buffer)
        return -1;

    q->capacity = cap;
    q->mask = cap - 1;

    atomic_init(&q->head, 0);
    atomic_init(&q->tail, 0);

    return 0;
}

void xlink_spsc_destroy(xlink_spsc_queue_t *q)
{
    if (q->buffer) {
        free(q->buffer);
        q->buffer = NULL;
    }
    q->capacity = 0;
    q->mask = 0;
}
