/*
 * aio_uring.c — Linux io_uring-based async I/O engine
 *
 * Uses raw io_uring syscalls (no liburing dependency).
 * Linux 5.1+ required for io_uring, Linux 5.6+ for IORING_OP_POLL_ADD.
 *
 * Step 2.7: xlink v2.1 — io_uring engine.
 */
#define _GNU_SOURCE
#include "aio.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <poll.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <linux/io_uring.h>
#include <stdatomic.h>
#include <time.h>
#include <fcntl.h>

/* ─── Raw syscall wrappers ────────────────────────────── */

static int io_uring_setup(uint32_t entries, struct io_uring_params *p) {
    return (int)syscall(__NR_io_uring_setup, entries, p);
}

static int io_uring_enter(int ring_fd, uint32_t to_submit,
                           uint32_t min_complete, uint32_t flags) {
    return (int)syscall(__NR_io_uring_enter, ring_fd, to_submit,
                        min_complete, flags, NULL, 0);
}

/* ─── Ring layout helpers ─────────────────────────────── */

struct io_uring_sq {
    uint32_t           *head;
    uint32_t           *tail;
    uint32_t           *ring_mask;
    uint32_t           *ring_entries;
    uint32_t           *flags;
    uint32_t           *dropped;
    uint32_t           *array;       /* sqe index → ring index mapping */
    struct io_uring_sqe *sqes;       /* submission queue entries */
    size_t              ring_sz;
    void               *ring_ptr;
};

struct io_uring_cq {
    uint32_t           *head;
    uint32_t           *tail;
    uint32_t           *ring_mask;
    uint32_t           *ring_entries;
    struct io_uring_cqe *cqes;
    uint32_t           *overflow;
    size_t              ring_sz;
    void               *ring_ptr;
};

struct aio_uring {
    int                  ring_fd;
    struct io_uring_sq   sq;
    struct io_uring_cq   cq;

    xlink_channel_t    **chans;       /* maps watch index → channel */
    void               **user_data;   /* maps watch index → user data */
    int                  cap;
    int                  nwatched;
    int                  max_idx;

    uint32_t             pending;     /* number of unsubmitted SQEs */
};

#define URING_INIT_CAP  64
#define URING_QUEUE_DEPTH 256

/* ─── Ring mmap ───────────────────────────────────────── */

static int mmap_ring(int ring_fd, struct io_uring_params *p,
                     struct io_uring_sq *sq, struct io_uring_cq *cq) {
    size_t sq_sz = (size_t)p->sq_off.array + p->sq_entries * sizeof(uint32_t);
    size_t cq_sz = (size_t)p->cq_off.cqes + p->cq_entries * sizeof(struct io_uring_cqe);

    /* Single mmap if IORING_FEAT_SINGLE_MMAP is supported */
    if (p->features & IORING_FEAT_SINGLE_MMAP) {
        size_t total = cq_sz > sq_sz ? cq_sz : sq_sz;
        void *ptr = mmap(NULL, total, PROT_READ | PROT_WRITE,
                         MAP_SHARED | MAP_POPULATE, ring_fd,
                         IORING_OFF_SQ_RING);
        if (ptr == MAP_FAILED) return -1;
        sq->ring_ptr = ptr;
        sq->ring_sz  = total;

        ptr = mmap(NULL, total, PROT_READ | PROT_WRITE,
                   MAP_SHARED | MAP_POPULATE, ring_fd,
                   IORING_OFF_CQ_RING);
        if (ptr == MAP_FAILED) {
            munmap(sq->ring_ptr, sq->ring_sz);
            return -1;
        }
        cq->ring_ptr = ptr;
        cq->ring_sz  = total;
    } else {
        void *ptr = mmap(NULL, sq_sz, PROT_READ | PROT_WRITE,
                         MAP_SHARED | MAP_POPULATE, ring_fd,
                         IORING_OFF_SQ_RING);
        if (ptr == MAP_FAILED) return -1;
        sq->ring_ptr = ptr;
        sq->ring_sz  = sq_sz;

        ptr = mmap(NULL, cq_sz, PROT_READ | PROT_WRITE,
                   MAP_SHARED | MAP_POPULATE, ring_fd,
                   IORING_OFF_CQ_RING);
        if (ptr == MAP_FAILED) {
            munmap(sq->ring_ptr, sq->ring_sz);
            return -1;
        }
        cq->ring_ptr = ptr;
        cq->ring_sz  = cq_sz;
    }

    /* Map SQEs */
    size_t sqe_sz = p->sq_entries * sizeof(struct io_uring_sqe);
    sq->sqes = mmap(NULL, sqe_sz, PROT_READ | PROT_WRITE,
                    MAP_SHARED | MAP_POPULATE, ring_fd,
                    IORING_OFF_SQES);
    if (sq->sqes == MAP_FAILED) {
        munmap(sq->ring_ptr, sq->ring_sz);
        munmap(cq->ring_ptr, cq->ring_sz);
        return -1;
    }

    /* Set up SQ pointers */
    sq->head         = (uint32_t *)((char *)sq->ring_ptr + p->sq_off.head);
    sq->tail         = (uint32_t *)((char *)sq->ring_ptr + p->sq_off.tail);
    sq->ring_mask    = (uint32_t *)((char *)sq->ring_ptr + p->sq_off.ring_mask);
    sq->ring_entries = (uint32_t *)((char *)sq->ring_ptr + p->sq_off.ring_entries);
    sq->flags        = (uint32_t *)((char *)sq->ring_ptr + p->sq_off.flags);
    sq->dropped      = (uint32_t *)((char *)sq->ring_ptr + p->sq_off.dropped);
    sq->array        = (uint32_t *)((char *)sq->ring_ptr + p->sq_off.array);

    /* Set up CQ pointers */
    cq->head         = (uint32_t *)((char *)cq->ring_ptr + p->cq_off.head);
    cq->tail         = (uint32_t *)((char *)cq->ring_ptr + p->cq_off.tail);
    cq->ring_mask    = (uint32_t *)((char *)cq->ring_ptr + p->cq_off.ring_mask);
    cq->ring_entries = (uint32_t *)((char *)cq->ring_ptr + p->cq_off.ring_entries);
    cq->cqes         = (struct io_uring_cqe *)((char *)cq->ring_ptr + p->cq_off.cqes);
    cq->overflow     = (uint32_t *)((char *)cq->ring_ptr + p->cq_off.overflow);

    return 0;
}

/* ─── SQ submission helper ────────────────────────────── */

static struct io_uring_sqe *get_sqe(struct io_uring_sq *sq) {
    uint32_t head = *sq->head;
    uint32_t next = *sq->tail + 1;
    uint32_t mask = *sq->ring_mask;
    if ((next - head) > *sq->ring_entries)
        return NULL;  /* full */
    return &sq->sqes[*sq->tail & mask];
}

static void submit_sqe(struct io_uring_sq *sq) {
    /* Write barrier before advancing tail */
    __atomic_thread_fence(__ATOMIC_RELEASE);
    uint32_t mask = *sq->ring_mask;
    sq->array[(*sq->tail) & mask] = (*sq->tail) & mask;
    (*sq->tail)++;
}

static void advance_cq(struct io_uring_cq *cq, uint32_t n) {
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    (*cq->head) += n;
}

/* ─── Lifecycle ───────────────────────────────────────── */

static int uring_init(xlink_aio_t *aio) {
    struct aio_uring *ur = calloc(1, sizeof(*ur));
    if (!ur) return -1;

    struct io_uring_params p;
    memset(&p, 0, sizeof(p));
    p.flags = 0;

    ur->ring_fd = io_uring_setup(URING_QUEUE_DEPTH, &p);
    if (ur->ring_fd < 0) {
        free(ur);
        return -1;
    }

    if (mmap_ring(ur->ring_fd, &p, &ur->sq, &ur->cq) < 0) {
        close(ur->ring_fd);
        free(ur);
        return -1;
    }

    ur->chans     = calloc(URING_INIT_CAP, sizeof(xlink_channel_t*));
    ur->user_data = calloc(URING_INIT_CAP, sizeof(void*));
    if (!ur->chans || !ur->user_data) {
        munmap(ur->sq.ring_ptr, ur->sq.ring_sz);
        munmap(ur->cq.ring_ptr, ur->cq.ring_sz);
        munmap(ur->sq.sqes, p.sq_entries * sizeof(struct io_uring_sqe));
        close(ur->ring_fd);
        free(ur->chans);
        free(ur->user_data);
        free(ur);
        return -1;
    }
    ur->cap = URING_INIT_CAP;

    aio->priv = ur;
    return 0;
}

static void uring_fini(xlink_aio_t *aio) {
    struct aio_uring *ur = (struct aio_uring *)aio->priv;
    if (!ur) return;
    /* Unmap rings — sizes were saved at init time.
     * SQEs size = queue_depth * sizeof(sqe), derived from cq entries
     * since we used uniform depth. Use a heuristic. */
    munmap(ur->sq.sqes, URING_QUEUE_DEPTH * sizeof(struct io_uring_sqe));
    munmap(ur->sq.ring_ptr, ur->sq.ring_sz);
    munmap(ur->cq.ring_ptr, ur->cq.ring_sz);
    close(ur->ring_fd);
    free(ur->chans);
    free(ur->user_data);
    free(ur);
    aio->priv = NULL;
}

/* ─── Watch / unwatch ─────────────────────────────────── */

static int uring_watch(xlink_aio_t *aio, xlink_channel_t *ch,
                        void *user_data) {
    struct aio_uring *ur = (struct aio_uring *)aio->priv;
    if (!ur || ch->fd < 0) return -1;

    /* Grow tables if needed */
    if (ur->nwatched >= ur->cap) {
        int newcap = ur->cap * 2;
        xlink_channel_t **nc = realloc(ur->chans,
                                       (size_t)newcap * sizeof(xlink_channel_t*));
        void **nu = realloc(ur->user_data,
                            (size_t)newcap * sizeof(void*));
        if (!nc || !nu) {
            free(nc); free(nu);
            return -1;
        }
        ur->chans = nc;
        ur->user_data = nu;
        ur->cap = newcap;
    }

    int idx = ur->nwatched++;

    /* IORING_OP_POLL_ADD: wait for EPOLLIN on fd */
    struct io_uring_sqe *sqe = get_sqe(&ur->sq);
    if (!sqe) return -1;

    sqe->opcode    = IORING_OP_POLL_ADD;
    sqe->fd        = ch->fd;
    sqe->poll_events = POLLIN | POLLHUP | POLLERR;
    sqe->user_data = (uint64_t)(uintptr_t)ch;  /* store ch for lookup */

    ur->chans[idx]     = ch;
    ur->user_data[idx] = user_data;
    if (idx >= ur->max_idx) ur->max_idx = idx + 1;

    submit_sqe(&ur->sq);
    ur->pending++;

    return idx;
}

static int uring_unwatch(xlink_aio_t *aio, xlink_channel_t *ch) {
    struct aio_uring *ur = (struct aio_uring *)aio->priv;
    if (!ur || ch->fd < 0) return -1;

    /* IORING_OP_POLL_REMOVE: cancel a previously added poll.
     * Use user_data = 0 (never used by watch) so the wait path can
     * skip POLL_REMOVE CQEs cleanly. */
    struct io_uring_sqe *sqe = get_sqe(&ur->sq);
    if (!sqe) return -1;

    sqe->opcode    = IORING_OP_POLL_REMOVE;
    sqe->fd        = ch->fd;
    sqe->addr      = (uint64_t)(uintptr_t)ch;
    sqe->user_data = 0;  /* sentinel — skip in wait path */

    submit_sqe(&ur->sq);
    ur->pending++;

    /* Flush the POLL_REMOVE CQE immediately so it doesn't leak
     * into the next wait() call. */
    int ret = io_uring_enter(ur->ring_fd, ur->pending, 1,
                              IORING_ENTER_GETEVENTS);
    ur->pending = 0;
    if (ret > 0) {
        /* Drain the CQE(s) produced by POLL_REMOVE.
         * user_data == 0 → skip, no-op. */
        uint32_t head = *ur->cq.head;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        uint32_t tail = *ur->cq.tail;
        if (head != tail) {
            advance_cq(&ur->cq, (uint32_t)(tail - head));
        }
    }

    /* Clear from table */
    for (int i = 0; i < ur->max_idx; i++) {
        if (ur->chans[i] == ch) {
            ur->chans[i] = NULL;
            ur->user_data[i] = NULL;
            return 0;
        }
    }
    return -1;
}

/* ─── Wait ────────────────────────────────────────────── */

static int uring_wait(xlink_aio_t *aio, int timeout_ms,
                       xlink_channel_t **out_ch, void **out_user) {
    struct aio_uring *ur = (struct aio_uring *)aio->priv;
    if (!ur) return -2;

    /* Submit any pending SQEs */
    if (ur->pending > 0) {
        int ret = io_uring_enter(ur->ring_fd, ur->pending, 0,
                                  IORING_ENTER_GETEVENTS);
        /* ret may be < 0 on EINTR/EAGAIN — that's OK, we retry below */
        (void)ret;
        ur->pending = 0;
    }

    uint32_t head = *ur->cq.head;
    uint32_t tail;

    /* Check if CQE already available */
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    tail = *ur->cq.tail;
    if (head != tail)
        goto process_cqe;

    /* Need to wait — use io_uring_enter */
    if (timeout_ms != 0) {
        uint32_t flags = IORING_ENTER_GETEVENTS;
        if (timeout_ms < 0) {
            /* Block until at least one CQE */
            int ret = io_uring_enter(ur->ring_fd, 0, 1, flags);
            if (ret < 0) {
                if (errno == EINTR) return -1;
                return -2;
            }
        } else {
            /* __kernel_timespec: 2x int64_t (sec, nsec) */
            int64_t kts[2];
            kts[0] = timeout_ms / 1000;
            kts[1] = (timeout_ms % 1000) * 1000000L;

            /* Use io_uring_getevents_arg via raw syscall for timeout.
             * io_uring_enter wrapper we defined only takes 4 args;
             * use direct syscall for the extended version. */
            struct {
                uint64_t sigmask;
                uint32_t sigmask_sz;
                uint32_t pad;
                uint64_t ts;
            } arg;
            arg.sigmask    = 0;
            arg.sigmask_sz = 0;
            arg.pad        = 0;
            arg.ts         = (uint64_t)(uintptr_t)kts;

            int ret = (int)syscall(__NR_io_uring_enter, ur->ring_fd,
                                   0, 1,
                                   IORING_ENTER_GETEVENTS |
                                   IORING_ENTER_EXT_ARG,
                                   &arg, sizeof(arg));
            if (ret < 0) {
                if (errno == EINTR || errno == ETIME) return -1;
                return -2;
            }
            if (ret == 0) return -1;  /* timeout: 0 CQEs */
        }

        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        tail = *ur->cq.tail;
        if (head == tail) return -1;  /* still no CQEs */
    } else {
        /* timeout_ms == 0: poll once, no CQEs means timeout */
        return -1;
    }

process_cqe: {
        struct io_uring_cqe *cqe = &ur->cq.cqes[head & *ur->cq.ring_mask];
        uint64_t ud = cqe->user_data;
        int res = cqe->res;
        advance_cq(&ur->cq, 1);

        if (res < 0 && res != -EAGAIN && res != -ECANCELED) {
            /* Error on the FD — still report it */
            return -1;
        }

        /* Find the channel by user_data (which is the ch pointer) */
        xlink_channel_t *ch = (xlink_channel_t *)(uintptr_t)ud;
        for (int i = 0; i < ur->max_idx; i++) {
            if (ur->chans[i] == ch) {
                if (out_ch)   *out_ch   = ch;
                if (out_user) *out_user = ur->user_data[i];
                return i;
            }
        }
        return -1;
    }
}

/* ─── Ops table ───────────────────────────────────────── */

const struct xlink_aio_ops uring_ops = {
    .name   = "io_uring",
    .init   = uring_init,
    .fini   = uring_fini,
    .watch  = uring_watch,
    .unwatch= uring_unwatch,
    .wait   = uring_wait,
};