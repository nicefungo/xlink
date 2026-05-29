/*
 * aio_poll.c — POSIX poll()-based fallback engine
 *
 * Always available.  Used when epoll/io_uring are not available
 * or when explicitly requested.
 */
#include "aio.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/poll.h>
#include <time.h>

typedef struct {
    xlink_channel_t **chans;
    void            **user_data;
    int               nwatched;
    int               cap;
} aio_poll_t;

#define POLL_INIT_CAP 16

static int poll_init(xlink_aio_t *aio) {
    aio_poll_t *pp = calloc(1, sizeof(*pp));
    if (!pp) return -1;

    pp->chans     = calloc(POLL_INIT_CAP, sizeof(xlink_channel_t*));
    pp->user_data = calloc(POLL_INIT_CAP, sizeof(void*));
    if (!pp->chans || !pp->user_data) {
        free(pp->chans); free(pp->user_data); free(pp);
        return -1;
    }
    pp->cap = POLL_INIT_CAP;

    aio->priv = pp;
    return 0;
}

static void poll_fini(xlink_aio_t *aio) {
    aio_poll_t *pp = (aio_poll_t *)aio->priv;
    if (!pp) return;
    free(pp->chans);
    free(pp->user_data);
    free(pp);
    aio->priv = NULL;
}

static int poll_watch(xlink_aio_t *aio, xlink_channel_t *ch,
                      void *user_data) {
    aio_poll_t *pp = (aio_poll_t *)aio->priv;
    if (!pp) return -1;

    if (pp->nwatched >= pp->cap) {
        int newcap = pp->cap * 2;
        xlink_channel_t **nc = realloc(pp->chans,
                                       (size_t)newcap * sizeof(xlink_channel_t*));
        void **nu = realloc(pp->user_data,
                            (size_t)newcap * sizeof(void*));
        if (!nc || !nu) { free(nc); free(nu); return -1; }
        pp->chans = nc;
        pp->user_data = nu;
        pp->cap = newcap;
    }

    int idx = pp->nwatched++;
    pp->chans[idx] = ch;
    pp->user_data[idx] = user_data;
    return idx;
}

static int poll_unwatch(xlink_aio_t *aio, xlink_channel_t *ch) {
    aio_poll_t *pp = (aio_poll_t *)aio->priv;
    if (!pp) return -1;

    for (int i = 0; i < pp->nwatched; i++) {
        if (pp->chans[i] == ch) {
            /* Lazy removal: compact by swapping last into this slot */
            pp->chans[i] = pp->chans[pp->nwatched - 1];
            pp->user_data[i] = pp->user_data[pp->nwatched - 1];
            pp->chans[pp->nwatched - 1] = NULL;
            pp->user_data[pp->nwatched - 1] = NULL;
            pp->nwatched--;
            return 0;
        }
    }
    return -1;
}

static int64_t current_ms_poll(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int poll_wait_fn(xlink_aio_t *aio, int timeout_ms,
                         xlink_channel_t **out_ch, void **out_user) {
    aio_poll_t *pp = (aio_poll_t *)aio->priv;
    if (!pp) return -2;

    if (pp->nwatched == 0) {
        errno = ENOTSUP;
        return -2;
    }

    /* Build pollfd array */
    int nfds = pp->nwatched;
    struct pollfd pfds[256];  /* stack-allocated for speed */
    if (nfds > 256) nfds = 256;

    for (int i = 0; i < nfds; i++) {
        pfds[i].fd      = pp->chans[i]->fd;
        pfds[i].events  = POLLIN | POLLHUP | POLLERR;
        pfds[i].revents = 0;
    }

    int rc;
    do {
        rc = poll(pfds, (nfds_t)nfds, timeout_ms);
    } while (rc < 0 && errno == EINTR);

    if (rc <= 0) return -1;

    for (int i = 0; i < nfds; i++) {
        if (pfds[i].revents & (POLLIN | POLLHUP | POLLERR)) {
            if (out_ch)   *out_ch   = pp->chans[i];
            if (out_user) *out_user = pp->user_data[i];
            return i;
        }
    }
    return -1;
}

static const struct xlink_aio_ops poll_ops = {
    .name   = "poll",
    .init   = poll_init,
    .fini   = poll_fini,
    .watch  = poll_watch,
    .unwatch= poll_unwatch,
    .wait   = poll_wait_fn,
};
