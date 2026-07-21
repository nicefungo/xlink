#include "xlink_internal.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/socket.h>
#include <sys/poll.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <time.h>
#include <stdint.h>
#include <stdio.h>

/* ═══════════════════════════════════════════════════════════
 * Backend registry (via plugin system)
 * ═══════════════════════════════════════════════════════════ */

/* All backends are registered via xlink_plugins_init() in plugin.c.
 * xlink_open() uses xlink_plugin_find_by_type() to locate backends. */

/* ─── Framing helpers (4-byte big-endian length prefix) ── */

static inline uint32_t read_u32_be(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] <<  8) |  (uint32_t)p[3];
}

static inline void write_u32_be(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >>  8);
    p[3] = (uint8_t)(v >>  0);
}

/* Read exactly `len` bytes from fd (blocking, unless NONBLOCK).
 *
 * For non-blocking fds: if EAGAIN occurs before any data is read,
 * returns -1 with errno == EAGAIN.  If partial data was consumed
 * before EAGAIN, we poll() and retry *instead of* returning partial
 * data -- this prevents framing desync (the framing layer would
 * interpret partial header bytes as a complete 4-byte header and
 * lose synchronization with the stream).
 *
 * Returns number of bytes read on success, -1 on error.
 */
static ssize_t read_exact(int fd, void* buf, size_t len) {
    uint8_t* p = (uint8_t*)buf;
    size_t   remain = len;
    while (remain > 0) {
        ssize_t n = read(fd, p, remain);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (p == (uint8_t*)buf) {
                    /* No data at all -- tell caller to try again */
                    return -1;
                }
                /* Partial data consumed — poll for more instead of
                 * returning partial, which would desync the framer. */
                struct pollfd pfd = { .fd = fd, .events = POLLIN };
                int prc;
                do {
                    prc = poll(&pfd, 1, -1);
                } while (prc < 0 && errno == EINTR);
                if (prc <= 0) return -1;
                continue;
            }
            return -1;
        }
        if (n == 0) break;
        p += n;
        remain -= (size_t)n;
    }
    return (ssize_t)(len - remain);
}

/* ─── Stream framing: receive one message ──────────────── */

static int frame_recv(xlink_channel_t* ch, void* buf, size_t* len) {
    (void)ch;
    uint8_t header[4];
    ssize_t n;

    n = read_exact(ch->fd, header, 4);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            *len = 0;
            return -1;    /* non-blocking: no data available */
        }
        snprintf(ch->errbuf, sizeof(ch->errbuf),
                 "frame_recv: read header: %s", strerror(errno));
        return -1;
    }
    if ((size_t)n < 4) {
        snprintf(ch->errbuf, sizeof(ch->errbuf),
                 "frame_recv: short header (%zd bytes)", n);
        return -1;
    }

    uint32_t payload_len = read_u32_be(header);
    if (payload_len > *len) {
        /* Discard the payload to maintain framing sync */
        size_t remaining = payload_len;
        while (remaining > 0) {
            uint8_t discard[4096];
            size_t to_read = remaining > sizeof(discard) ? sizeof(discard) : remaining;
            n = read_exact(ch->fd, discard, to_read);
            if (n <= 0) {
                snprintf(ch->errbuf, sizeof(ch->errbuf),
                         "frame_recv: message too large (%u > %zu), partial discard",
                         payload_len, *len);
                return -1;
            }
            remaining -= (size_t)n;
        }
        snprintf(ch->errbuf, sizeof(ch->errbuf),
                 "frame_recv: message too large (%u > %zu), discarded",
                 payload_len, *len);
        return -1;
    }

    n = read_exact(ch->fd, buf, payload_len);
    if (n < 0) {
        snprintf(ch->errbuf, sizeof(ch->errbuf),
                 "frame_recv: read payload: %s", strerror(errno));
        return -1;
    }
    if ((size_t)n < payload_len) {
        snprintf(ch->errbuf, sizeof(ch->errbuf),
                 "frame_recv: short payload (%zd < %u)",
                 n, payload_len);
        return -1;
    }
    *len = (size_t)n;
    return 0;
}

static int frame_send(xlink_channel_t* ch, const void* data, size_t len) {
    uint8_t header[4];
    write_u32_be(header, (uint32_t)len);

    struct iovec iov[2];
    iov[0].iov_base = header;
    iov[0].iov_len  = 4;
    iov[1].iov_base = (void*)data;
    iov[1].iov_len  = len;

    size_t total = 0;
    while (total < 4 + len) {
        ssize_t n = writev(ch->fd, iov, 2);
        if (n < 0) {
            if (errno == EINTR) continue;
            snprintf(ch->errbuf, sizeof(ch->errbuf),
                     "frame_send: %s", strerror(errno));
            return -1;
        }
        total += (size_t)n;
        if ((size_t)n < iov[0].iov_len) {
            iov[0].iov_base = (char*)iov[0].iov_base + n;
            iov[0].iov_len -= (size_t)n;
        } else {
            size_t rem = (size_t)n - iov[0].iov_len;
            iov[0].iov_len = 0;
            iov[1].iov_base = (char*)iov[1].iov_base + rem;
            iov[1].iov_len -= rem;
        }
    }
    return 0;
}

/* ═══════════════════════════════════════════════════════════
 * SHM cleanup registry (atexit) — prevent dangling shm segments
 * ═══════════════════════════════════════════════════════════ */

/* Forward declaration — linked from libshm_ipc.a */
extern int shm_destroy(const char* name);

#define MAX_CLEANUP 256

static char*  cleanup_names[MAX_CLEANUP];
static int    cleanup_count  = 0;
static int    cleanup_done   = 0;
static int    cleanup_regd   = 0;

static void shm_cleanup_all(void) {
    if (cleanup_done) return;
    cleanup_done = 1;

    for (int i = 0; i < cleanup_count; i++) {
        if (cleanup_names[i]) {
            shm_destroy(cleanup_names[i]);
            free(cleanup_names[i]);
            cleanup_names[i] = NULL;
        }
    }
    cleanup_count = 0;
}

void xlink_register_shm_cleanup(const char* name) {
    if (!cleanup_regd) {
        atexit(shm_cleanup_all);
        cleanup_regd = 1;
    }

    if (cleanup_count >= MAX_CLEANUP) return;

    cleanup_names[cleanup_count] = strdup(name);
    if (cleanup_names[cleanup_count])
        cleanup_count++;
}

/* ═══════════════════════════════════════════════════════════
 * Multi-channel wait
 * ═══════════════════════════════════════════════════════════ */

static int64_t current_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + (int64_t)ts.tv_nsec / 1000000;
}

int xlink_wait(xlink_channel_t** chans, int n, int timeout_ms) {
    if (n <= 0 || !chans) { errno = EINVAL; return -2; }

    for (int i = 0; i < n; i++) {
        if (!chans[i]) {
            errno = EINVAL;
            return -2;
        }
    }

    /* Strategy: collect pollable fds + SHM-peek-capable channels.
     * If ALL channels have fds → single poll() call.
     * Otherwise → periodic poll/peek loop. */
    struct pollfd* pfds = NULL;
    int*           map  = NULL;   /* pfd index → chan index */
    int            npfd = 0;

    /* First pass: count pollable fds */
    for (int i = 0; i < n; i++) {
        if (chans[i]->fd >= 0)
            npfd++;
    }

    if (npfd > 0) {
        pfds = calloc((size_t)npfd, sizeof(struct pollfd));
        map  = calloc((size_t)npfd, sizeof(int));
        if (!pfds || !map) {
            free(pfds); free(map);
            errno = ENOMEM;
            return -2;
        }

        int idx = 0;
        for (int i = 0; i < n; i++) {
            if (chans[i]->fd >= 0) {
                pfds[idx].fd     = chans[i]->fd;
                pfds[idx].events = POLLIN;
                pfds[idx].revents= 0;
                map[idx] = i;
                idx++;
            }
        }
    }

    /*
     * All channels have real fds → single poll()
     */
    if (npfd == n) {
        int rc;
        do {
            rc = poll(pfds, (nfds_t)npfd, timeout_ms);
        } while (rc < 0 && errno == EINTR);
        if (rc <= 0) {
            free(pfds); free(map);
            return -1;
        }

        int ret = -1;
        for (int i = 0; i < npfd; i++) {
            if (pfds[i].revents & (POLLIN | POLLHUP | POLLERR)) {
                ret = map[i];
                break;
            }
        }
        free(pfds); free(map);
        return ret;
    }

    /*
     * Mixed / no-fd channels → periodic loop
     */
    int has_peek = 0;
    for (int i = 0; i < n; i++)
        if (chans[i]->backend->peek)
            has_peek = 1;

    if (!has_peek && npfd == 0) {
        /* Can't wait on any channel */
        free(pfds); free(map);
        errno = ENOTSUP;
        return -2;
    }

    int64_t deadline_ms = (timeout_ms < 0)
                          ? INT64_MAX
                          : (current_ms() + timeout_ms);

    for (;;) {
        /* Poll fds — do work first, check deadline after */
        if (npfd > 0) {
            int remain;
            if (deadline_ms == INT64_MAX) {
                remain = 10;
            } else {
                int64_t elapsed = current_ms();
                remain = (elapsed >= deadline_ms) ? 0
                        : (int)(deadline_ms - elapsed);
                if (remain > 100) remain = 100;
            }

            int rc;
            do {
                rc = poll(pfds, (nfds_t)npfd, remain);
            } while (rc < 0 && errno == EINTR);
            if (rc > 0) {
                for (int i = 0; i < npfd; i++) {
                    if (pfds[i].revents & (POLLIN | POLLHUP | POLLERR)) {
                        int ret = map[i];
                        free(pfds); free(map);
                        return ret;
                    }
                }
            }
        }

        /* Peek SHM-like channels */
        for (int i = 0; i < n; i++) {
            if (chans[i]->fd < 0 && chans[i]->backend->peek) {
                size_t avail = 0;
                if (chans[i]->backend->peek(chans[i], &avail) == 0
                    && avail > 0) {
                    free(pfds); free(map);
                    return i;
                }
            }
        }

        /* Brief sleep before retry (if no pollable fds) */
        if (npfd == 0)
            usleep(5000);  /* 5ms */

        /* Check deadline — at least one poll+peek cycle done.
         * This check is intentionally AFTER the work, so timeout=0
         * still does one poll+peek before returning. */
        if (timeout_ms >= 0 && current_ms() >= deadline_ms) {
            free(pfds); free(map);
            return -1;
        }
    }
}

/* ═══════════════════════════════════════════════════════════
 * Public API
 * ═══════════════════════════════════════════════════════════ */

xlink_channel_t* xlink_open(xlink_type_t type, const char* addr,
                            const xlink_opt_t* opt) {
    /* Ensure built-in plugins are registered (lazy init) */
    xlink_plugins_init();

    const xlink_plugin_t *pl = xlink_plugin_find_by_type(type);
    const xlink_backend_t *bk = pl ? pl->backend : NULL;
    if (!bk) {
        errno = ENOSYS;
        return NULL;
    }

    xlink_channel_t* ch = calloc(1, sizeof(*ch));
    if (!ch) return NULL;

    ch->backend = bk;
    ch->fd      = -1;
    ch->flags   = opt ? (int)opt->flags : 0;
    ch->opt     = opt ? *opt : (xlink_opt_t)XLINK_OPT_DEFAULT;
    ch->errbuf[0] = '\0';

    ch->use_framing = (type == XLINK_PIPE || type == XLINK_TCP
                       || type == XLINK_SERIAL);

    if (bk->open(ch, addr, opt) != 0) {
        int saved = errno;
        free(ch);
        errno = saved;
        return NULL;
    }

    return ch;
}

int xlink_send(xlink_channel_t* ch, const void* data, size_t len) {
    if (ch->use_framing)
        return frame_send(ch, data, len);
    return ch->backend->send(ch, data, len);
}

/* ─── Adaptive Batching: internal helpers ──────────────── */

/* EWMA alpha: ~0.125 — converges in ~8 samples */
#define BATCH_EWMA_ALPHA  0.125

static double timespec_to_us(const struct timespec *ts) {
    return (double)ts->tv_sec * 1e6 + (double)ts->tv_nsec / 1e3;
}

static void batch_ewma_update(struct xlink_batch_state *bs,
                               int count, int total_bytes) {
    double msg_rate = 0.0;
    double avg_size = (double)total_bytes / (double)count;

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    double elapsed_us = timespec_to_us(&now) - timespec_to_us(&bs->last_flush);
    if (elapsed_us > 0.0)
        msg_rate = (double)count / (elapsed_us / 1e6);

    if (bs->samples == 0) {
        bs->avg_msg_rate = msg_rate;
        bs->avg_msg_size = avg_size;
    } else {
        bs->avg_msg_rate = BATCH_EWMA_ALPHA * msg_rate
                         + (1.0 - BATCH_EWMA_ALPHA) * bs->avg_msg_rate;
        bs->avg_msg_size = BATCH_EWMA_ALPHA * avg_size
                         + (1.0 - BATCH_EWMA_ALPHA) * bs->avg_msg_size;
    }
    bs->samples++;
}

static int batch_decide_flush(struct xlink_batch_state *bs) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    /* Hard limit: max batch reached */
    if (bs->q_count >= bs->policy.max_batch)
        return 1;

    /* Adaptive logic: only when enabled and enough samples */
    if (bs->policy.enable && bs->samples >= 8) {
        /* Large messages: reduce batch size to minimize delay */
        if (bs->avg_msg_size >= 4096.0) {
            bs->current_batch = bs->policy.max_batch / 4;
            if (bs->current_batch < 1) bs->current_batch = 1;
        }
        /* High frequency: increase batch size */
        else if (bs->avg_msg_rate > 10000.0) {
            bs->current_batch = bs->policy.max_batch;
        }
        /* Medium frequency: moderate batch */
        else if (bs->avg_msg_rate > 1000.0) {
            bs->current_batch = bs->policy.max_batch / 2;
            if (bs->current_batch < bs->policy.min_batch)
                bs->current_batch = bs->policy.min_batch;
        } else {
            bs->current_batch = bs->policy.min_batch;
        }

        /* Time-based flush: max_delay exceeded */
        double elapsed_us = timespec_to_us(&now)
                          - timespec_to_us(&bs->first_queued);
        if (elapsed_us >= (double)bs->policy.max_delay_us)
            return 1;

        /* Threshold-based flush */
        if (bs->q_count >= bs->current_batch)
            return 1;
    }

    /* Non-adaptive mode: flush when above min_batch or hard limit */
    if (bs->q_count >= bs->policy.min_batch)
        return 1;

    return 0;   /* defer */
}

static int batch_flush_internal(xlink_channel_t *ch) {
    struct xlink_batch_state *bs = ch->bs;
    if (!bs || bs->q_count == 0) return 0;

    int corked = 0;
    if (ch->use_framing && ch->fd >= 0) {
        int stype = 0;
        socklen_t slen = sizeof(stype);
        if (getsockopt(ch->fd, SOL_SOCKET, SO_TYPE,
                       &stype, &slen) == 0 && stype == SOCK_STREAM) {
            int on = 1;
            if (setsockopt(ch->fd, IPPROTO_TCP, TCP_CORK,
                           &on, sizeof(on)) == 0)
                corked = 1;
        }
    }

    int total_bytes = 0, sent = 0;
    while (bs->q_count > 0) {
        xlink_msg_t *m = &bs->queue[bs->q_tail];
        int rc;
        if (ch->use_framing)
            rc = frame_send(ch, m->data, m->len);
        else
            rc = ch->backend->send(ch, m->data, m->len);
        if (rc != 0) {
            if (corked) {
                int off = 0;
                setsockopt(ch->fd, IPPROTO_TCP, TCP_CORK,
                           &off, sizeof(off));
            }
            return sent;   /* partial: remaining msgs stay in queue */
        }
        total_bytes += (int)m->len;
        bs->q_tail = (bs->q_tail + 1) % XLINK_BATCH_QCAP;
        bs->q_count--;
        sent++;
    }

    if (corked) {
        int off = 0;
        setsockopt(ch->fd, IPPROTO_TCP, TCP_CORK, &off, sizeof(off));
    }

    /* Update EWMA after flush */
    batch_ewma_update(bs, sent, total_bytes);
    clock_gettime(CLOCK_MONOTONIC, &bs->last_flush);
    return sent;
}

int xlink_send_batch(xlink_channel_t* ch,
                     const xlink_msg_t* msgs, int count) {
    if (!ch || !msgs || count <= 0) return -1;

    /* Adaptive batch path: queue messages, flush when policy triggers */
    if (ch->bs) {
        int queued = 0;
        for (int i = 0; i < count; i++) {
            if (ch->bs->q_count >= XLINK_BATCH_QCAP) {
                /* Queue full: try flush, then retry one slot */
                batch_flush_internal(ch);
                if (ch->bs->q_count >= XLINK_BATCH_QCAP)
                    break;
            }
            if (ch->bs->q_count == 0)
                clock_gettime(CLOCK_MONOTONIC, &ch->bs->first_queued);
            int wp = ch->bs->q_head;
            ch->bs->queue[wp] = msgs[i];
            ch->bs->q_head = (wp + 1) % XLINK_BATCH_QCAP;
            ch->bs->q_count++;
            queued++;
        }

        /* Check if policy says flush now */
        if (queued > 0 && batch_decide_flush(ch->bs))
            batch_flush_internal(ch);

        return queued;
    }

    /* ── Lock-free SPSC path (SHM backend, thread-safe) ── */
    if (ch->lfq && ch->backend->type == XLINK_SHM) {
        int queued = 0;
        for (int i = 0; i < count; i++) {
            /* Copy message data — caller retains ownership of msgs[] */
            xlink_msg_t *m = malloc(sizeof(*m) + msgs[i].len);
            if (!m) break;
            m->data = (char *)(m + 1);
            m->len  = msgs[i].len;
            memcpy((void *)m->data, msgs[i].data, msgs[i].len);
            if (xlink_spsc_enqueue((xlink_spsc_queue_t *)ch->lfq, m) != 0) {
                free(m);
                break;  /* queue full */
            }
            queued++;
        }
        return queued;
    }

    /* ── Standard (non-adaptive) path ── */

    /* TCP_CORK coalesces multiple writev() calls into fewer TCP segments.
     * Enabled only for SOCK_STREAM fds (TCP), has no effect on other backends. */
    int tcp_corked = 0;
    if (ch->use_framing && ch->fd >= 0) {
        int stype = 0;
        socklen_t slen = sizeof(stype);
        if (getsockopt(ch->fd, SOL_SOCKET, SO_TYPE,
                       &stype, &slen) == 0 && stype == SOCK_STREAM) {
            int on = 1;
            if (setsockopt(ch->fd, IPPROTO_TCP, TCP_CORK,
                           &on, sizeof(on)) == 0)
                tcp_corked = 1;
        }
    }

    int sent = 0;
    for (int i = 0; i < count; i++) {
        int rc;
        if (ch->use_framing)
            rc = frame_send(ch, msgs[i].data, msgs[i].len);
        else
            rc = ch->backend->send(ch, msgs[i].data, msgs[i].len);
        if (rc != 0) {
            if (tcp_corked) {
                int off = 0;
                setsockopt(ch->fd, IPPROTO_TCP, TCP_CORK,
                           &off, sizeof(off));
            }
            return sent;
        }
        sent++;
    }

    if (tcp_corked) {
        int off = 0;
        setsockopt(ch->fd, IPPROTO_TCP, TCP_CORK, &off, sizeof(off));
    }

    return sent;
}

int xlink_recv_batch(xlink_channel_t* ch,
                     xlink_msg_t* msgs, int count) {
    if (!ch || !msgs || count <= 0) return -1;

    int recvd = 0;
    for (int i = 0; i < count; i++) {
        if (!msgs[i].data || msgs[i].len == 0) return -1;

        /* Non-destructive peek: bail if no data waiting */
        if (ch->use_framing) {
            size_t avail = 0;
            if (ch->backend->peek && ch->backend->peek(ch, &avail) == 0
                && avail == 0)
                break;
        }

        size_t sz = msgs[i].len;
        int rc;
        if (ch->use_framing)
            rc = frame_recv(ch, (void *)msgs[i].data, &sz);
        else
            rc = ch->backend->recv(ch, (void *)msgs[i].data, &sz);
        if (rc != 0) break;   /* no more data or error */
        msgs[i].len = sz;
        recvd++;
    }
    return recvd;
}

int xlink_set_batch_policy(xlink_channel_t *ch,
                           const xlink_batch_policy_t *policy) {
    if (!ch || !policy) return -1;
    if (policy->max_batch <= 0 || policy->max_batch > XLINK_BATCH_QCAP)
        return -1;

    /* Lazy-allocate batch state */
    if (!ch->bs) {
        ch->bs = calloc(1, sizeof(*ch->bs));
        if (!ch->bs) return -1;
        clock_gettime(CLOCK_MONOTONIC, &ch->bs->first_queued);
        ch->bs->current_batch = policy->max_batch / 2;
        if (ch->bs->current_batch < policy->min_batch)
            ch->bs->current_batch = policy->min_batch;
    }

    ch->bs->policy = *policy;
    return 0;
}

int xlink_flush_batch(xlink_channel_t *ch) {
    if (!ch || !ch->bs) return 0;
    return batch_flush_internal(ch);
}

/* ─── Lock-free SPSC send queue (SHM backend) ─── */

int xlink_lfq_init(xlink_channel_t *ch, size_t capacity) {
    if (!ch) return -1;
    if (ch->backend->type != XLINK_SHM) return -1;
    if (ch->lfq) return 0;  /* already initialized */

    xlink_spsc_queue_t *q = calloc(1, sizeof(*q));
    if (!q) return -1;
    if (xlink_spsc_init(q, capacity) != 0) {
        free(q);
        return -1;
    }
    ch->lfq = q;
    return 0;
}

int xlink_lfq_flush(xlink_channel_t *ch) {
    if (!ch || !ch->lfq) return 0;

    xlink_spsc_queue_t *q = (xlink_spsc_queue_t *)ch->lfq;
    void *item;

    /* Dequeue exactly one message and send it.  SHM backend uses
     * shm_writen() which blocks until the consumer reads the previous
     * message — so we must NOT loop here.  Callers should call
     * lfq_flush() repeatedly until it returns 0 or -1. */
    if (xlink_spsc_dequeue(q, &item) != 0)
        return 0;

    xlink_msg_t *m = (xlink_msg_t *)item;
    int rc;
    if (ch->use_framing)
        rc = frame_send(ch, m->data, m->len);
    else
        rc = ch->backend->send(ch, m->data, m->len);
    free(m);
    if (rc != 0) {
        snprintf(ch->errbuf, sizeof(ch->errbuf),
                 "lfq_flush send failed");
        return -1;
    }
    return 1;
}

size_t xlink_lfq_count(xlink_channel_t *ch) {
    if (!ch || !ch->lfq) return 0;
    return xlink_spsc_count((xlink_spsc_queue_t *)ch->lfq);
}

int xlink_recv(xlink_channel_t* ch, void* buf, size_t* len) {
    if (ch->use_framing)
        return frame_recv(ch, buf, len);
    return ch->backend->recv(ch, buf, len);
}

int xlink_write(xlink_channel_t* ch, const void* data, size_t len) {
    if (ch->backend->write)
        return ch->backend->write(ch, data, len);
    return ch->backend->send(ch, data, len);
}

int xlink_read(xlink_channel_t* ch, void* buf, size_t len, int timeout_ms) {
    if (ch->backend->read)
        return ch->backend->read(ch, buf, len, timeout_ms);
    size_t n = len;
    if (ch->backend->recv(ch, buf, &n) == 0)
        return (int)n;
    return -1;
}

int xlink_peek(xlink_channel_t* ch, size_t* avail) {
    if (ch->backend->peek)
        return ch->backend->peek(ch, avail);
    *avail = 0;
    return 0;
}

void xlink_close(xlink_channel_t* ch) {
    if (!ch) return;
#ifdef XLINK_HAS_TLS
    xlink_tls_cleanup(ch);
#endif
    if (ch->lfq) {
        /* Drain remaining messages to avoid leaks */
        xlink_spsc_queue_t *q = (xlink_spsc_queue_t *)ch->lfq;
        void *item;
        while (xlink_spsc_dequeue(q, &item) == 0)
            free(item);
        xlink_spsc_destroy(q);
        free(q);
    }
    free(ch->bs);   /* free adaptive batch state if allocated */
    ch->backend->close(ch);
    free(ch);
}

const char* xlink_errstr(xlink_channel_t* ch) {
    if (!ch) return strerror(errno);
    return ch->errbuf[0] ? ch->errbuf : strerror(errno);
}

xlink_channel_t* xlink_open_url(const char *url,
                                const xlink_opt_t *opt) {
    xlink_plugins_init();

    /* Extract protocol: "proto://rest" → "proto" + "rest" */
    const char *sep = strstr(url, "://");
    if (!sep) return NULL;

    size_t proto_len = (size_t)(sep - url);
    if (proto_len == 0 || proto_len > 31) return NULL;

    char proto[32];
    memcpy(proto, url, proto_len);
    proto[proto_len] = '\0';

    const xlink_plugin_t *pl = xlink_plugin_find(proto);
    if (!pl) {
        errno = ENOSYS;
        return NULL;
    }

    return xlink_open(pl->proto, sep + 3, opt);
}

const char* xlink_type_str(xlink_type_t t) {
    xlink_plugins_init();
    const xlink_plugin_t *pl = xlink_plugin_find_by_type(t);
    if (pl) return pl->name;
    return "unknown";
}

/* Public ⟷ internal plugin bridge */
size_t xlink_plugin_count(void) {
    return xlink_plugin_count_impl();
}

void xlink_dump(xlink_channel_t* ch, int fd) {
    if (!ch) {
        const char* msg = "xlink channel @ (null)\n";
        size_t sl = strlen(msg);
        ssize_t dummy_ = write(fd, msg, sl);
        (void)dummy_;
        return;
    }
    char line[256];
    int wn = snprintf(line, sizeof(line),
                      "xlink channel @ %p\n"
                      "  type:   %s\n"
                      "  fd:     %d\n"
                      "  flags:  0x%x\n"
                      "  framing:%s\n"
                      "  err:    %s\n",
                      (void*)ch,
                      ch->backend->name,
                      ch->fd,
                      ch->flags,
                      ch->use_framing ? "yes" : "no",
                      xlink_errstr(ch));
    if (wn > 0) {
        size_t towrite = (size_t)(wn < (int)sizeof(line) - 1
                                  ? wn : (int)sizeof(line) - 1);
        ssize_t dummy  = write(fd, line, towrite);
        (void)dummy;
    }
}
