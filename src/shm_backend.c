#include "xlink_internal.h"
#include "shm_ipc.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>

/*
 * SHM backend wraps the existing name-based shm_ipc library.
 *
 * shm_ipc API:
 *   shm_create(name)              — compete (single consumer)
 *   shm_create_broadcast(name, n) — broadcast (N consumers)
 *   shm_write(name, data, len)    — fire-and-forget send
 *   shm_read(name, buf, &len)     — non-blocking recv
 *   shm_peek(name, &seq, &len)    — check availability
 *   shm_destroy(name)             — unlink & cleanup
 *
 * xlink convention:
 *   XLINK_CREATE → init segment
 *   no XLINK_CREATE → just read/write (segment must exist)
 *   close does NOT destroy (caller manages lifetime)
 */

typedef struct {
    char name[64];
} shm_priv_t;

static int shm_backend_open(xlink_channel_t* ch, const char* addr,
                            const xlink_opt_t* opt) {
    shm_priv_t* p = calloc(1, sizeof(*p));
    if (!p) return -1;

    size_t n = strlen(addr);
    if (n >= sizeof(p->name)) n = sizeof(p->name) - 1;
    memcpy(p->name, addr, n);
    p->name[n] = '\0';

    int flags = opt ? (int)opt->flags : 0;

    if (flags & XLINK_CREATE) {
        int is_bcast = (flags & XLINK_BROADCAST) ? 1 : 0;
        int rc;
        if (is_bcast)
            rc = shm_create_broadcast(p->name, 16);
        else
            rc = shm_create(p->name);
        if (rc != 0) {
            free(p);
            return -1;
        }
        /* Register for atexit cleanup */
        xlink_register_shm_cleanup(p->name);
    }

    ch->priv = p;
    return 0;
}

static void shm_backend_close(xlink_channel_t* ch) {
    if (!ch->priv) return;
    /* Don't destroy — caller manages lifetime */
    free(ch->priv);
    ch->priv = NULL;
}

static int shm_backend_send(xlink_channel_t* ch, const void* data, size_t len) {
    shm_priv_t* p = (shm_priv_t*)ch->priv;
    if (shm_writen(p->name, data, len) != 0) {
        snprintf(ch->errbuf, sizeof(ch->errbuf),
                 "shm_write(%s): %s", p->name, strerror(errno));
        return -1;
    }
    return 0;
}

static int shm_backend_recv(xlink_channel_t* ch, void* buf, size_t* len) {
    shm_priv_t* p = (shm_priv_t*)ch->priv;
    int rc;
    if (ch->flags & XLINK_NONBLOCK)
        rc = shm_read(p->name, buf, len);
    else
        rc = shm_readn(p->name, buf, len);
    if (rc != 0) {
        /* shm_read returns -1 for "no data" (non-blocking empty queue).
         * shm_readn returns -1 on error with errno set. */
        if ((ch->flags & XLINK_NONBLOCK) && rc == -1)
            snprintf(ch->errbuf, sizeof(ch->errbuf),
                     "shm_read(%s): no data", p->name);
        else
            snprintf(ch->errbuf, sizeof(ch->errbuf),
                     "shm_read(%s): %s", p->name, strerror(errno));
        return -1;
    }
    return 0;
}

static int shm_backend_peek(xlink_channel_t* ch, size_t* avail) {
    shm_priv_t* p = (shm_priv_t*)ch->priv;
    size_t len;
    if (shm_peek(p->name, NULL, &len) != 0) {
        *avail = 0;
        return 0;
    }
    *avail = len;
    return 0;
}

const xlink_backend_t xlink_shm_backend = {
    .type  = XLINK_SHM,
    .name  = "shm",
    .open  = shm_backend_open,
    .close = shm_backend_close,
    .send  = shm_backend_send,
    .recv  = shm_backend_recv,
    .write = NULL,
    .read  = NULL,
    .peek  = shm_backend_peek,
};
