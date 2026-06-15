/*
 * aio.c — async I/O engine registry and xlink_wait_aio()
 *
 * v2.1: SHM channels now have eventfd FIFOs (step 2.5),
 *       eliminating the usleep() peek fallback path.
 */
#include "aio.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>

/* Forward declarations from engine files */
extern const struct xlink_aio_ops poll_ops;

#ifdef __linux__
extern const struct xlink_aio_ops epoll_ops;
#endif

/* ─── Engine creation ─────────────────────────────────── */

xlink_aio_t *xlink_aio_create_impl(xlink_aio_type_t type) {
    xlink_aio_t *aio = calloc(1, sizeof(*aio));
    if (!aio) return NULL;

#ifdef __linux__
    if (type == XLINK_AIO_EPOLL || type == XLINK_AIO_AUTO) {
        aio->ops = &epoll_ops;
        if (aio->ops->init(aio) == 0) {
            aio->type = XLINK_AIO_EPOLL;
            return aio;
        }
        /* epoll failed — fall through to poll */
    }
#else
    (void)epoll_ops;  /* suppress unused warning */
#endif

    /* Default: POSIX poll */
    aio->ops = &poll_ops;
    if (aio->ops->init(aio) == 0) {
        aio->type = XLINK_AIO_POLL;
        return aio;
    }

    free(aio);
    return NULL;
}

void xlink_aio_destroy_impl(xlink_aio_t *aio) {
    if (!aio) return;
    if (aio->ops) aio->ops->fini(aio);
    free(aio);
}

const char *xlink_aio_name_impl(xlink_aio_t *aio) {
    if (!aio || !aio->ops) return "none";
    return aio->ops->name;
}

/* Drain notification bytes from a SHM event FIFO.
 * Only called for SHM channels whose fd is the FIFO read end,
 * NOT for pipe/tcp/etc whose fd is the actual data source. */
static void drain_notify_fd(int fd) {
    uint8_t c;
    /* Drain exactly 1 byte — one notification per event.
     * If we drain all, we lose the count and epoll won't wake
     * for remaining SHM data. */
    ssize_t n = read(fd, &c, 1);
    (void)n;  /* best-effort drain */
}

/* Check if fd needs drain (SHM notification FIFO).
 * Heuristic: backend has .peek → fd is notification FIFO, needs drain.
 *             backend has no .peek → fd is real I/O, don't touch. */
static int needs_drain(xlink_channel_t *ch) {
    return ch->fd >= 0 && ch->backend && ch->backend->peek;
}

/* ─── xlink_wait_aio() — event-driven multi-channel wait ─ */

int xlink_wait_aio_impl(xlink_channel_t **chans, int n,
                   int timeout_ms, xlink_aio_t *aio) {
    if (!chans || n <= 0 || !aio) return -2;

    int has_peek = 0;  /* any channels without fd that need peek? */

    /* Register all channels with fd >= 0 */
    for (int i = 0; i < n; i++) {
        if (chans[i]->fd >= 0) {
            aio->ops->watch(aio, chans[i], (void *)(intptr_t)i);
        } else if (chans[i]->backend && chans[i]->backend->peek) {
            has_peek = 1;
        }
    }

    /* Fast path: all channels have fds — single wait, no loop */
    if (!has_peek) {
        xlink_channel_t *ready_ch = NULL;
        void *user = NULL;
        int rc = aio->ops->wait(aio, timeout_ms, &ready_ch, &user);

        if (rc >= 0 && ready_ch) {
            int idx = (int)(intptr_t)user;

            /* Drain SHM notification FIFO (not real I/O fds) */
            if (needs_drain(chans[idx]))
                drain_notify_fd(chans[idx]->fd);

            /* Verify SHM data is actually there (handle spurious wake) */
            if (chans[idx]->backend && chans[idx]->backend->peek) {
                size_t avail = 0;
                if (chans[idx]->backend->peek(chans[idx], &avail) != 0
                    || avail == 0) {
                    for (int j = 0; j < n; j++)
                        if (chans[j]->fd >= 0)
                            aio->ops->unwatch(aio, chans[j]);
                    return -1;
                }
            }

            /* Unregister all */
            for (int j = 0; j < n; j++)
                if (chans[j]->fd >= 0)
                    aio->ops->unwatch(aio, chans[j]);
            return idx;
        }

        /* rc < 0 (timeout or error) — clean up and return */
        for (int j = 0; j < n; j++)
            if (chans[j]->fd >= 0)
                aio->ops->unwatch(aio, chans[j]);
        return rc;
    }

    /* Mixed path: some channels have fd, some need peek.
     * Use a polling loop for the peek-only channels. */
    struct timespec start_ts;
    if (timeout_ms >= 0)
        clock_gettime(CLOCK_MONOTONIC, &start_ts);

    for (;;) {
        xlink_channel_t *ready_ch = NULL;
        void *user = NULL;
        int slice_ms = timeout_ms >= 0
            ? (timeout_ms > 100 ? 100 : timeout_ms)
            : 100;

        int rc = aio->ops->wait(aio, slice_ms, &ready_ch, &user);
        if (rc >= 0 && ready_ch) {
            int idx = (int)(intptr_t)user;
            if (needs_drain(chans[idx]))
                drain_notify_fd(chans[idx]->fd);
            /* Verify SHM data */
            if (chans[idx]->backend && chans[idx]->backend->peek) {
                size_t avail = 0;
                if (chans[idx]->backend->peek(chans[idx], &avail) != 0
                    || avail == 0) {
                    continue;  /* spurious — keep waiting */
                }
            }
            for (int j = 0; j < n; j++)
                if (chans[j]->fd >= 0)
                    aio->ops->unwatch(aio, chans[j]);
            return idx;
        }

        /* Check peek-only channels */
        for (int i = 0; i < n; i++) {
            if (chans[i]->fd < 0 && chans[i]->backend && chans[i]->backend->peek) {
                size_t avail = 0;
                if (chans[i]->backend->peek(chans[i], &avail) == 0
                    && avail > 0) {
                    for (int j = 0; j < n; j++)
                        if (chans[j]->fd >= 0)
                            aio->ops->unwatch(aio, chans[j]);
                    return i;
                }
            }
        }

        /* Timeout handling */
        if (timeout_ms >= 0) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            int64_t elapsed = (now.tv_sec - start_ts.tv_sec) * 1000
                            + (now.tv_nsec - start_ts.tv_nsec) / 1000000;
            if (elapsed >= timeout_ms) {
                for (int j = 0; j < n; j++)
                    if (chans[j]->fd >= 0)
                        aio->ops->unwatch(aio, chans[j]);
                return -1;
            }
        }
    }
}

/* ─── Public API bridges (void* handles) ─── */

void *xlink_aio_create(int type) {
    return (void *)xlink_aio_create_impl((xlink_aio_type_t)type);
}

void xlink_aio_destroy(void *engine) {
    xlink_aio_destroy_impl((xlink_aio_t *)engine);
}

const char *xlink_aio_name(void *engine) {
    return xlink_aio_name_impl((xlink_aio_t *)engine);
}

int xlink_wait_aio(xlink_channel_t **chans, int n,
                   int timeout_ms, void *aio_engine) {
    return xlink_wait_aio_impl(chans, n, timeout_ms,
                             (xlink_aio_t *)aio_engine);
}

/* ─── xlink_run() — event-driven main loop ────────────── */

int xlink_run(xlink_channel_t **chans, int n,
              int timeout_ms, void *aio_engine,
              xlink_callback_t cb, void *arg) {
    if (!chans || n <= 0 || !cb) return -2;

    xlink_aio_t *aio = (xlink_aio_t *)aio_engine;
    if (!aio) {
        /* Auto-create default engine if not provided */
        aio = xlink_aio_create_impl(XLINK_AIO_AUTO);
        if (!aio) return -2;
    }

    struct timespec start_ts;
    int remaining_ms = timeout_ms;
    int last_stale_idx = -1;  /* prev peek idx whose data wasn't consumed */

    if (timeout_ms > 0)
        clock_gettime(CLOCK_MONOTONIC, &start_ts);

    for (;;) {
        /* First check: any channel with pending peek data?
         * Skip the channel that already had peek data last round
         * and whose data wasn't consumed — prevents busy-loop when
         * the callback doesn't drain the channel. */
        int peek_idx = -1;
        for (int i = 0; i < n; i++) {
            if (i == last_stale_idx) continue;  /* skip — data is stale */
            if (chans[i]->backend && chans[i]->backend->peek) {
                size_t avail = 0;
                if (chans[i]->backend->peek(chans[i], &avail) == 0
                    && avail > 0) {
                    peek_idx = i;
                    break;
                }
            }
        }

        if (peek_idx >= 0) {
            /* Data already buffered — deliver to callback without waiting */
            int rc = cb(chans, n, peek_idx, arg);
            if (rc != 0) {
                if (aio != (xlink_aio_t *)aio_engine)
                    xlink_aio_destroy_impl(aio);
                return 0;
            }
            last_stale_idx = peek_idx;  /* data likely not consumed */
            continue;
        }

        /* No buffered data — need to wait for new arrival.
         * Reset stale tracker: a real wait_aio event breaks the peek-loop. */
        last_stale_idx = -1;
        int slice_ms = (remaining_ms < 0) ? -1 :
                       (remaining_ms > 100 ? 100 : remaining_ms);

        int idx = xlink_wait_aio_impl(chans, n, slice_ms, aio);
        if (idx >= 0) {
            int rc = cb(chans, n, idx, arg);
            if (rc != 0) {
                if (aio != (xlink_aio_t *)aio_engine)
                    xlink_aio_destroy_impl(aio);
                return 0;
            }
        } else if (idx == -1) {
            /* Timeout on this slice — check overall timeout */
            if (timeout_ms >= 0) {
                struct timespec now;
                clock_gettime(CLOCK_MONOTONIC, &now);
                int64_t elapsed = (now.tv_sec - start_ts.tv_sec) * 1000
                                + (now.tv_nsec - start_ts.tv_nsec) / 1000000;
                if (elapsed >= timeout_ms) {
                    if (aio != (xlink_aio_t *)aio_engine)
                        xlink_aio_destroy_impl(aio);
                    return -1;
                }
                remaining_ms = timeout_ms - (int)elapsed;
            }
        } else {
            /* idx == -2: engine error */
            if (aio != (xlink_aio_t *)aio_engine)
                xlink_aio_destroy_impl(aio);
            return -2;
        }
    }
}