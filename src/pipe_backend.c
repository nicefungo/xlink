#include "xlink_internal.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

/*
 * Pipe (named FIFO) backend.
 *
 * Opens the FIFO in O_RDWR mode so the same channel can both send and recv.
 * The framing layer in xlink.c handles message boundaries via 4-byte prefix.
 *
 * XLINK_CREATE → mkfifo() if not exist, then open O_RDWR
 * no  CREATE  → open O_RDWR (FIFO must already exist)
 */

static int pipe_backend_open(xlink_channel_t* ch, const char* addr,
                             const xlink_opt_t* opt) {
    int flags = opt ? (int)opt->flags : 0;

    if (flags & XLINK_CREATE) {
        /* Create FIFO if it doesn't exist */
        if (mkfifo(addr, 0666) != 0 && errno != EEXIST) {
            snprintf(ch->errbuf, sizeof(ch->errbuf),
                     "mkfifo(%s): %s", addr, strerror(errno));
            return -1;
        }
    }

    ch->fd = open(addr, O_RDWR);
    if (ch->fd < 0) {
        snprintf(ch->errbuf, sizeof(ch->errbuf),
                 "open(%s): %s", addr, strerror(errno));
        return -1;
    }

    if (flags & XLINK_NONBLOCK) {
        int fl = fcntl(ch->fd, F_GETFL, 0);
        fcntl(ch->fd, F_SETFL, fl | O_NONBLOCK);
    }

    ch->use_framing = 1;
    return 0;
}

static void pipe_backend_close(xlink_channel_t* ch) {
    if (ch->fd >= 0) {
        close(ch->fd);
        ch->fd = -1;
    }
}

static int pipe_backend_send(xlink_channel_t* ch, const void* data, size_t len) {
    /* fallback: raw write if framing not used */
    const uint8_t* p = (const uint8_t*)data;
    size_t remain = len;
    while (remain > 0) {
        ssize_t n;
        do { n = write(ch->fd, p, remain); } while (n < 0 && errno == EINTR);
        if (n <= 0) {
            snprintf(ch->errbuf, sizeof(ch->errbuf),
                     "pipe write: %s", strerror(errno));
            return -1;
        }
        p += n;
        remain -= (size_t)n;
    }
    return 0;
}

static int pipe_backend_recv(xlink_channel_t* ch, void* buf, size_t* len) {
    /* fallback: raw read if framing not used */
    ssize_t n;
    do { n = read(ch->fd, buf, *len); } while (n < 0 && errno == EINTR);
    if (n <= 0) {
        snprintf(ch->errbuf, sizeof(ch->errbuf),
                 "pipe read: %s", n == 0 ? "EOF" : strerror(errno));
        return -1;
    }
    *len = (size_t)n;
    return 0;
}

const xlink_backend_t xlink_pipe_backend = {
    .type  = XLINK_PIPE,
    .name  = "pipe",
    .open  = pipe_backend_open,
    .close = pipe_backend_close,
    .send  = pipe_backend_send,
    .recv  = pipe_backend_recv,
    .write = NULL,
    .read  = NULL,
    .peek  = NULL,
};
