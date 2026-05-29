/*
 * aio.c — async I/O engine registry and xlink_wait_aio()
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

/* ─── xlink_wait_aio() — event-driven multi-channel wait ─ */

int xlink_wait_aio_impl(xlink_channel_t **chans, int n,
                   int timeout_ms, xlink_aio_t *aio) {
    if (!chans || n <= 0 || !aio) return -2;

    /* Register all channels with fd >= 0 */
    for (int i = 0; i < n; i++) {
        if (chans[i]->fd < 0) continue;  /* SHM handled by peek fallback */
        aio->ops->watch(aio, chans[i], (void *)(intptr_t)i);
    }

    /* Main loop: wait for events */
    for (;;) {
        xlink_channel_t *ready_ch = NULL;
        void *user = NULL;
        int remain_ms = timeout_ms;

        int rc = aio->ops->wait(aio, remain_ms, &ready_ch, &user);
        if (rc >= 0 && ready_ch) {
            int idx = (int)(intptr_t)user;
            /* Unregister all */
            for (int j = 0; j < n; j++)
                if (chans[j]->fd >= 0)
                    aio->ops->unwatch(aio, chans[j]);
            return idx;
        }

        /* Check SHM-like channels (no fd) via peek */
        int64_t start = 0;
        if (timeout_ms >= 0) {
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            start = ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
        }

        for (int i = 0; i < n; i++) {
            if (chans[i]->fd < 0 && chans[i]->backend->peek) {
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

        /* Timeout check for SHM-only case */
        if (timeout_ms >= 0) {
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            int64_t now = ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
            if (now - start >= timeout_ms) {
                for (int j = 0; j < n; j++)
                    if (chans[j]->fd >= 0)
                        aio->ops->unwatch(aio, chans[j]);
                return -1;
            }
        }

        /* If wait returned -1 (timeout) and we had no fds, loop */
        if (rc == -1 && timeout_ms < 0) {
            usleep(5000);
            continue;
        }
        if (rc == -1) break;  /* timeout with observable state */
    }

    for (int j = 0; j < n; j++)
        if (chans[j]->fd >= 0)
            aio->ops->unwatch(aio, chans[j]);
    return -1;
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
