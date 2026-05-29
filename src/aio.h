/*
 * aio.h — async I/O engine abstraction (internal)
 *
 * This header is NOT part of the public API.  The public API
 * uses void* handles (xlink_aio_create/destroy in xlink.h).
 */
#ifndef XLINK_AIO_H
#define XLINK_AIO_H

#include "xlink_internal.h"
#include <stddef.h>

/* ─── Engine type ─────────────────────────────────────── */

typedef enum {
    XLINK_AIO_AUTO    = 0,
    XLINK_AIO_POLL,       /* POSIX poll — always available */
    XLINK_AIO_EPOLL,      /* Linux epoll */
    XLINK_AIO_IOURING,    /* Linux io_uring */
} xlink_aio_type_t;

/* ─── Async operation kind ────────────────────────────── */

typedef enum {
    XLINK_OP_READ,
    XLINK_OP_WRITE,
    XLINK_OP_PEEK,    /* used for SHM notification */
} xlink_aio_op_kind_t;

/* ─── Completion callback ─────────────────────────────── */

typedef void (*xlink_aio_cb)(xlink_channel_t *ch,
                             xlink_aio_op_kind_t kind,
                             int result,
                             void *user_data);

/* ─── Engine handle ───────────────────────────────────── */

typedef struct xlink_aio xlink_aio_t;

struct xlink_aio_ops {
    const char *name;

    /* Lifecycle */
    int  (*init)(xlink_aio_t *aio);
    void (*fini)(xlink_aio_t *aio);

    /* Register/unregister a channel's fd for event notification.
     * Used by xlink_wait_aio(). */
    int  (*watch)(xlink_aio_t *aio, xlink_channel_t *ch, void *user_data);
    int  (*unwatch)(xlink_aio_t *aio, xlink_channel_t *ch);

    /* Wait for events on registered channels.
     * timeout_ms: -1 = forever, 0 = poll once, >0 = timeout.
     * Returns: 0..n-1 = which channel (via user_data), -1 = timeout,
     *          -2 = error.
     */
    int  (*wait)(xlink_aio_t *aio, int timeout_ms,
                 xlink_channel_t **out_ch, void **out_user);
};

struct xlink_aio {
    xlink_aio_type_t      type;
    const struct xlink_aio_ops *ops;
    void                 *priv;       /* engine-specific data */
};

/* ─── Public API (internal → public bridge in aio.c) ──── */

/* These match the public API declared in xlink.h (void* handles).
 * The internal impl uses typed xlink_aio_t pointers. */

xlink_aio_t *xlink_aio_create_impl(xlink_aio_type_t type);
void         xlink_aio_destroy_impl(xlink_aio_t *aio);
const char  *xlink_aio_name_impl(xlink_aio_t *aio);

/* Wait for data on n channels using the async engine.
 * Drop-in replacement for xlink_wait(). */
int          xlink_wait_aio_impl(xlink_channel_t **chans, int n,
                                 int timeout_ms, xlink_aio_t *aio);

#endif /* XLINK_AIO_H */
