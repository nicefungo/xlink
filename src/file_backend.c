#include "xlink_internal.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>

/*
 * File backend — record & replay.
 *
 * Open modes (determined by flags):
 *   XLINK_CREATE  → create/truncate for writing (record mode)
 *   no XLINK_CREATE → open existing for reading (replay mode)
 *
 * No framing: each xlink_read() returns file bytes directly.
 * Messages are the raw file contents from current position to EOF.
 *
 * Useful for:
 *   - Recording messages for later replay
 *   - Feeding test data from pre-recorded files
 *   - Capturing output for analysis
 */

static int file_backend_open(xlink_channel_t* ch, const char* addr,
                             const xlink_opt_t* opt) {
    int flags = opt ? (int)opt->flags : 0;
    int oflags = (flags & XLINK_CREATE) ? (O_WRONLY | O_CREAT | O_TRUNC)
                                        : O_RDONLY;

    ch->fd = open(addr, oflags, 0666);
    if (ch->fd < 0) {
        snprintf(ch->errbuf, sizeof(ch->errbuf),
                 "file: open(%s): %s", addr, strerror(errno));
        return -1;
    }

    ch->use_framing = 0;

    if (flags & XLINK_NONBLOCK) {
        int fl = fcntl(ch->fd, F_GETFL, 0);
        fcntl(ch->fd, F_SETFL, fl | O_NONBLOCK);
    }

    return 0;
}

static void file_backend_close(xlink_channel_t* ch) {
    if (ch->fd >= 0) {
        close(ch->fd);
        ch->fd = -1;
    }
}

static int file_backend_send(xlink_channel_t* ch, const void* data, size_t len) {
    const uint8_t* p = (const uint8_t*)data;
    size_t remain = len;
    while (remain > 0) {
        ssize_t n;
        do { n = write(ch->fd, p, remain); } while (n < 0 && errno == EINTR);
        if (n <= 0) {
            snprintf(ch->errbuf, sizeof(ch->errbuf),
                     "file write: %s", errno ? strerror(errno) : "short write");
            return -1;
        }
        p += n;
        remain -= (size_t)n;
    }
    return 0;
}

static int file_backend_recv(xlink_channel_t* ch, void* buf, size_t* len) {
    ssize_t n;
    do { n = read(ch->fd, buf, *len); } while (n < 0 && errno == EINTR);
    if (n <= 0) {
        if (n == 0) {
            snprintf(ch->errbuf, sizeof(ch->errbuf),
                     "file: EOF");
        } else {
            snprintf(ch->errbuf, sizeof(ch->errbuf),
                     "file read: %s", strerror(errno));
        }
        return -1;
    }
    *len = (size_t)n;
    return 0;
}

static int file_backend_read(xlink_channel_t* ch, void* buf, size_t len, int timeout_ms) {
    /* Regular file poll always returns immediately (files are always "ready").
     * Timeout is effectively ignored, but this matches xlink_read() semantics
     * for non-seekable reads. */
    struct pollfd pfd = { .fd = ch->fd, .events = POLLIN };
    int rc;
    do { rc = poll(&pfd, 1, timeout_ms); } while (rc < 0 && errno == EINTR);
    if (rc <= 0) {
        if (rc == 0) errno = ETIMEDOUT;
        return -1;
    }
    size_t n = len;
    int ret = file_backend_recv(ch, buf, &n);
    return (ret == 0) ? (int)n : -1;
}

const xlink_backend_t xlink_file_backend = {
    .type  = XLINK_FILE,
    .name  = "file",
    .open  = file_backend_open,
    .close = file_backend_close,
    .send  = file_backend_send,
    .recv  = file_backend_recv,
    .write = NULL,
    .read  = file_backend_read,
    .peek  = NULL,
};
