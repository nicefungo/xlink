/*
 * aio_epoll.c — Linux epoll-based async I/O engine
 *
 * This is the preferred engine on Linux 2.6+ when io_uring is
 * not available (or not desired).
 */
#include "aio.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <time.h>

/* ─── Private state ───────────────────────────────────── */

typedef struct {
    int            epfd;
    int            nwatched;          /* number of registered channels */
    int            cap;               /* allocated slots              */

    /* Just register fd→channel mapping. epoll_data_t stores our index. */
    xlink_channel_t **chans;          /* maps user_data (index) → channel */
    void           **user_data;       /* maps index → user callback data  */
    int              max_idx;         /* highest used index + 1           */
} aio_epoll_t;

#define EPOLL_INIT_CAP  16
#define EPOLL_MAX_EVENTS 64

/* ─── Lifecycle ───────────────────────────────────────── */

static int epoll_init(xlink_aio_t *aio) {
    aio_epoll_t *ep = calloc(1, sizeof(*ep));
    if (!ep) return -1;

    ep->epfd = epoll_create1(EPOLL_CLOEXEC);
    if (ep->epfd < 0) {
        free(ep);
        return -1;
    }

    ep->chans     = calloc(EPOLL_INIT_CAP, sizeof(xlink_channel_t*));
    ep->user_data = calloc(EPOLL_INIT_CAP, sizeof(void*));
    if (!ep->chans || !ep->user_data) {
        close(ep->epfd);
        free(ep->chans);
        free(ep->user_data);
        free(ep);
        return -1;
    }
    ep->cap = EPOLL_INIT_CAP;

    aio->priv = ep;
    return 0;
}

static void epoll_fini(xlink_aio_t *aio) {
    aio_epoll_t *ep = (aio_epoll_t *)aio->priv;
    if (!ep) return;
    close(ep->epfd);
    free(ep->chans);
    free(ep->user_data);
    free(ep);
    aio->priv = NULL;
}

/* ─── Channel registration ────────────────────────────── */

static int epoll_watch(xlink_aio_t *aio, xlink_channel_t *ch,
                       void *user_data) {
    aio_epoll_t *ep = (aio_epoll_t *)aio->priv;
    if (!ep || ch->fd < 0) return -1;

    /* Grow tables if needed */
    if (ep->nwatched >= ep->cap) {
        int newcap = ep->cap * 2;
        xlink_channel_t **nc = realloc(ep->chans,
                                       (size_t)newcap * sizeof(xlink_channel_t*));
        void **nu = realloc(ep->user_data,
                            (size_t)newcap * sizeof(void*));
        if (!nc || !nu) {
            free(nc); free(nu);
            return -1;
        }
        ep->chans = nc;
        ep->user_data = nu;
        ep->cap = newcap;
    }

    int idx = ep->nwatched++;

    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events   = EPOLLIN | EPOLLHUP | EPOLLERR;
    ev.data.u32 = (uint32_t)idx;

    if (epoll_ctl(ep->epfd, EPOLL_CTL_ADD, ch->fd, &ev) < 0)
        return -1;

    ep->chans[idx] = ch;
    ep->user_data[idx] = user_data;
    if (idx >= ep->max_idx) ep->max_idx = idx + 1;

    return idx;
}

static int epoll_unwatch(xlink_aio_t *aio, xlink_channel_t *ch) {
    aio_epoll_t *ep = (aio_epoll_t *)aio->priv;
    if (!ep || ch->fd < 0) return -1;

    /* Find the channel in our table */
    for (int i = 0; i < ep->max_idx; i++) {
        if (ep->chans[i] == ch) {
            epoll_ctl(ep->epfd, EPOLL_CTL_DEL, ch->fd, NULL);
            ep->chans[i] = NULL;
            ep->user_data[i] = NULL;
            return 0;
        }
    }
    return -1;
}

/* ─── Wait ────────────────────────────────────────────── */

static int epoll_wait_fn(xlink_aio_t *aio, int timeout_ms,
                          xlink_channel_t **out_ch, void **out_user) {
    aio_epoll_t *ep = (aio_epoll_t *)aio->priv;
    if (!ep) return -2;

    struct epoll_event events[EPOLL_MAX_EVENTS];

    int nfds = epoll_wait(ep->epfd, events, EPOLL_MAX_EVENTS, timeout_ms);
    if (nfds < 0) {
        if (errno == EINTR) return -1;  /* signal → treat as timeout */
        return -2;
    }
    if (nfds == 0) return -1;           /* timeout */

    /* Return first ready channel */
    for (int i = 0; i < nfds; i++) {
        int idx = (int)events[i].data.u32;
        if (idx >= 0 && idx < ep->max_idx && ep->chans[idx]) {
            if (out_ch)   *out_ch   = ep->chans[idx];
            if (out_user) *out_user = ep->user_data[idx];
            return idx;
        }
    }
    return -1;
}

/* ─── Ops table ───────────────────────────────────────── */

const struct xlink_aio_ops epoll_ops = {
    .name   = "epoll",
    .init   = epoll_init,
    .fini   = epoll_fini,
    .watch  = epoll_watch,
    .unwatch= epoll_unwatch,
    .wait   = epoll_wait_fn,
};
