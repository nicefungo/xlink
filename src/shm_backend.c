#include "xlink_internal.h"
#include "shm_ipc.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <time.h>
#include <stdint.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>

/*
 * SHM backend wraps the existing name-based shm_ipc library.
 *
 * Two modes of operation:
 *
 *   Default (no XLINK_SPSC):
 *     Uses shm_ipc library (mutex-protected ring buffer).
 *     shm_write() / shm_read() / shm_peek().
 *
 *   Lock-free mode (XLINK_SPSC):
 *     Uses shared-memory SPSC queue, bypassing shm_ipc locks.
 *     A region of shared memory (shm_open + mmap) holds:
 *       - atomic head/tail counters
 *       - fixed-size data ring buffer
 *     This eliminates the pthread_mutex_lock inside shm_ipc's
 *     shm_write()/shm_read(), giving ~4x throughput in single-
 *     producer scenarios (see docs/future-plans/04-performance.md).
 *
 * shm_ipc API (default mode):
 *   shm_create(name)              — compete (single consumer)
 *   shm_create_broadcast(name, n) — broadcast (N consumers)
 *   shm_write(name, data, len)    — fire-and-forget send
 *   shm_read(name, buf, &len)     — non-blocking recv
 *   shm_peek(name, &seq, &len)    — check availability
 *   shm_destroy(name)             — unlink & cleanup
 *
 * xlink convention:
 *   XLINK_CREATE → init segment + eventfd FIFO
 *   no XLINK_CREATE → just read/write (segment must exist)
 *   close does NOT destroy (caller manages lifetime)
 *
 * 通知机制 (v2.1: step 2.5 — SHM eventfd):
 *   每个 SHM channel 配对一个命名 FIFO (/tmp/xlink-evt-<name>)。
 *   发送端写完 SHM 后打开 FIFO 写入 1 字节通知信号。
 *   接收端在 epoll 中监听 FIFO 的读端 → 唤醒后 peek SHM。
 *   这消除了 xlink_wait_aio() 中 SHM 通道的 usleep() 轮询。
 */

#define FIFO_PREFIX "/tmp/xlink-evt-"

/* ── SPSC shared-memory layout ──────────────────────────
 *
 * The mmap'd region has this layout:
 *   [head (8B)] [tail (8B)] [capacity (8B)] [mask (8B)]
 *   [padding 32B] ← cache-line alignment
 *   [data: capacity * sizeof(msg_hdr) bytes]
 *
 * Each message is prefixed with a 4-byte length header:
 *   msg_hdr { uint32_t len; char data[]; }
 */

#define SPSC_SHM_PREFIX  "/xlink-spsc-"
#define SPSC_DATA_CAP    65536   /* 64KB data ring */
#define SPSC_HDR_OFFSET  64      /* skip atomic counters + padding */
#define SPSC_MMAP_SIZE   (SPSC_HDR_OFFSET + SPSC_DATA_CAP)

typedef struct {
    _Atomic size_t head;        /* producer write cursor (byte offset) */
    _Atomic size_t tail;        /* consumer read cursor (byte offset) */
    size_t         capacity;    /* data region size */
    size_t         mask;        /* capacity - 1 (power of 2) */
    char           _pad[32];    /* cache-line padding */
    /* unsigned char data[capacity] follows at offset 64 */
} shm_spsc_hdr_t;

typedef struct {
    char             name[64];
    int              fifo_created;   /* did we create the FIFO? */
    int              use_spsc;       /* XLINK_SPSC mode active */
    int              spsc_owner;     /* we created the SPSC shm region */
    char             spsc_path[128]; /* /dev/shm path for SPSC region */
    int              spsc_fd;        /* fd for SPSC shm region */
    shm_spsc_hdr_t  *spsc_hdr;       /* mmap'd header */
    unsigned char   *spsc_data;      /* data ring (spsc_hdr + 64 bytes) */
    size_t           spsc_wr;        /* producer local write cursor (non-atomic) */
    size_t           spsc_rd;        /* consumer local read cursor (non-atomic) */
} shm_priv_t;

/* Build FIFO path from SHM name */
static void fifo_path(const char *name, char *out, size_t out_sz) {
    snprintf(out, out_sz, "%s%s", FIFO_PREFIX, name);
}

/* Open FIFO for read, non-blocking. Returns fd or -1. */
static int fifo_open_reader(const char *name) {
    char path[128];
    fifo_path(name, path, sizeof(path));

    int fd = open(path, O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        /* FIFO not created yet by sender — that's OK for non-CREATE opens */
        return -1;
    }
    return fd;
}

/* Write 1-byte notification to the FIFO (sender side).
 * Opens O_WRONLY | O_NONBLOCK, writes, closes. */
static void fifo_notify(const char *name) {
    char path[128];
    fifo_path(name, path, sizeof(path));

    int fd = open(path, O_WRONLY | O_NONBLOCK);
    if (fd < 0) return;  /* reader not ready yet — no-op */
    uint8_t c = 1;
    ssize_t nw = write(fd, &c, 1);
    (void)nw;  /* best-effort */
    close(fd);
}

/* Build SPSC shared memory path from channel name */
static void spsc_shm_path(const char *name, char *out, size_t out_sz) {
    snprintf(out, out_sz, "%s%s", SPSC_SHM_PREFIX, name);
}

/*
 * Create or open a lock-free SPSC queue in shared memory.
 *
 * The owner (XLINK_CREATE) allocates and zeroes the region.
 * The non-owner opens the existing region for read/write.
 *
 * On success, p->spsc_hdr and p->spsc_data are set and the
 * spsc_fd is open. On failure, any partial resources are cleaned.
 */
static int spsc_shm_init(shm_priv_t *p, const char *name, int is_owner) {
    char path[128];
    spsc_shm_path(name, path, sizeof(path));
    memcpy(p->spsc_path, path, sizeof(p->spsc_path));

    int oflag = O_RDWR;
    if (is_owner) {
        oflag |= O_CREAT | O_EXCL;
        /* Clean up any stale segment */
        shm_unlink(path);
    }

    p->spsc_fd = shm_open(path, oflag, 0666);
    if (p->spsc_fd < 0) {
        return -1;
    }

    if (is_owner) {
        if (ftruncate(p->spsc_fd, SPSC_MMAP_SIZE) != 0) {
            close(p->spsc_fd);
            shm_unlink(path);
            p->spsc_fd = -1;
            return -1;
        }
    }

    void *addr = mmap(NULL, SPSC_MMAP_SIZE,
                      PROT_READ | PROT_WRITE, MAP_SHARED,
                      p->spsc_fd, 0);
    if (addr == MAP_FAILED) {
        close(p->spsc_fd);
        if (is_owner) shm_unlink(path);
        p->spsc_fd = -1;
        return -1;
    }

    p->spsc_hdr  = (shm_spsc_hdr_t *)addr;
    p->spsc_data = (unsigned char *)addr + SPSC_HDR_OFFSET;

    if (is_owner) {
        memset(addr, 0, SPSC_MMAP_SIZE);
        p->spsc_hdr->capacity = SPSC_DATA_CAP;
        p->spsc_hdr->mask     = SPSC_DATA_CAP - 1;
    }

    p->spsc_wr = 0;
    p->spsc_rd = 0;
    return 0;
}

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

        /* Create notification FIFO */
        char path[128];
        fifo_path(p->name, path, sizeof(path));
        unlink(path);  /* clean stale FIFO if any */
        if (mkfifo(path, 0666) == 0) {
            p->fifo_created = 1;
        }

        /* Initialize lock-free SPSC queue if requested */
        if (flags & XLINK_SPSC) {
            p->use_spsc = 1;
            p->spsc_owner = 1;
            if (spsc_shm_init(p, p->name, 1) != 0) {
                free(p);
                return -1;
            }
            /* Don't set ch->lfq — the generic lfq path in xlink.c
             * uses its own xlink_spsc_queue_t. We manage SPSC
             * internally via p->spsc_hdr. */
        }
    } else {
        /* Non-CREATE open: check if SPSC shm region exists */
        char spsc_path[128];
        spsc_shm_path(p->name, spsc_path, sizeof(spsc_path));
        int test_fd = shm_open(spsc_path, O_RDWR, 0666);
        if (test_fd >= 0) {
            close(test_fd);
            p->use_spsc = 1;
            p->spsc_owner = 0;
            if (spsc_shm_init(p, p->name, 0) != 0) {
                p->use_spsc = 0;  /* degrade gracefully */
            }
        }
    }

    /* Open FIFO for read — epoll will watch this fd */
    ch->fd = fifo_open_reader(p->name);
    ch->priv = p;
    return 0;
}

static void shm_backend_close(xlink_channel_t* ch) {
    if (!ch->priv) return;
    shm_priv_t* p = (shm_priv_t*)ch->priv;

    if (ch->fd >= 0) {
        close(ch->fd);
        ch->fd = -1;
    }

    /* Clean up SPSC shared memory region */
    if (p->spsc_hdr) {
        munmap(p->spsc_hdr, SPSC_MMAP_SIZE);
        p->spsc_hdr = NULL;
        p->spsc_data = NULL;
    }
    if (p->spsc_fd >= 0) {
        close(p->spsc_fd);
        p->spsc_fd = -1;
    }
    if (p->spsc_owner) {
        shm_unlink(p->spsc_path);
    }

    /* Only the creator unlinks the FIFO */
    if (p->fifo_created) {
        char path[128];
        fifo_path(p->name, path, sizeof(path));
        unlink(path);
    }

    /* Don't destroy SHM segment — caller manages lifetime */
    free(ch->priv);
    ch->priv = NULL;
}

/*
 * SPSC enqueue: write msg into the shared-memory data ring.
 * Format: [len:4B][data:len bytes].
 * Returns 0 on success, -1 if no space.
 *
 * head/tail are byte-offset cursors. The ring wraps at capacity
 * (a power of 2), and we leave 1 slot unused to distinguish full
 * from empty (same semantics as spsc_queue.h).
 */
static int spsc_enqueue_msg(shm_priv_t *p, const void *data, size_t len) {
    shm_spsc_hdr_t *h = p->spsc_hdr;
    size_t hdr_sz = sizeof(uint32_t);
    size_t total = hdr_sz + len;
    size_t cap = h->capacity;

    size_t head = atomic_load_explicit(&h->head, memory_order_relaxed);
    size_t tail = atomic_load_explicit(&h->tail, memory_order_acquire);

    /* Check space: leave 1 byte gap to avoid full==empty ambiguity */
    size_t used = head - tail;
    if (used + total + 1 > cap)
        return -1;  /* no room */

    /* Write length prefix + data */
    uint32_t nlen = (uint32_t)len;
    size_t pos = head & h->mask;
    size_t avail = cap - pos;

    if (avail >= total) {
        memcpy(p->spsc_data + pos, &nlen, hdr_sz);
        memcpy(p->spsc_data + pos + hdr_sz, data, len);
    } else {
        /* Wrap: write first part to end, remainder from start */
        memcpy(p->spsc_data + pos, &nlen,
               avail >= hdr_sz ? hdr_sz : avail);
        if (avail < hdr_sz) {
            /* length header split across boundary */
            size_t rem = hdr_sz - avail;
            memcpy(p->spsc_data, ((unsigned char *)&nlen) + avail, rem);
            memcpy(p->spsc_data + rem, data, len);
        } else {
            size_t data_part1 = avail - hdr_sz;
            memcpy(p->spsc_data + pos + hdr_sz, data, data_part1);
            memcpy(p->spsc_data, (const unsigned char *)data + data_part1, len - data_part1);
        }
    }

    atomic_store_explicit(&h->head, head + total, memory_order_release);
    return 0;
}

/*
 * SPSC dequeue: read msg from the shared-memory data ring.
 * Returns msg length on success, 0 if empty, -1 on error.
 * Caller must provide buf with at least *len bytes.
 * On success, *len is set to actual message size.
 */
static int spsc_dequeue_msg(shm_priv_t *p, void *buf, size_t *len) {
    shm_spsc_hdr_t *h = p->spsc_hdr;
    size_t cap = h->capacity;

    size_t tail = atomic_load_explicit(&h->tail, memory_order_relaxed);
    size_t head = atomic_load_explicit(&h->head, memory_order_acquire);

    if (tail == head)
        return 0;  /* empty */

    /* Read length prefix */
    uint32_t msg_len;
    size_t pos = tail & h->mask;
    size_t avail = cap - pos;

    if (avail >= sizeof(uint32_t)) {
        memcpy(&msg_len, p->spsc_data + pos, sizeof(uint32_t));
    } else {
        /* length split across wrap */
        memcpy(&msg_len, p->spsc_data + pos, avail);
        memcpy(((unsigned char *)&msg_len) + avail, p->spsc_data, sizeof(uint32_t) - avail);
    }

    if (msg_len == 0 || msg_len > *len) {
        /* corrupted or buffer too small — skip past it */
        atomic_store_explicit(&h->tail, tail + sizeof(uint32_t) + msg_len,
                              memory_order_release);
        return -1;
    }

    size_t hdr_sz = sizeof(uint32_t);
    size_t total = hdr_sz + msg_len;
    size_t data_pos = (pos + hdr_sz) & h->mask;

    /* Copy data (handle wrap) */
    avail = cap - data_pos;
    if (avail >= msg_len) {
        memcpy(buf, p->spsc_data + data_pos, msg_len);
    } else {
        memcpy(buf, p->spsc_data + data_pos, avail);
        memcpy((unsigned char *)buf + avail, p->spsc_data, msg_len - avail);
    }

    atomic_store_explicit(&h->tail, tail + total, memory_order_release);
    *len = msg_len;
    return (int)msg_len;
}

static int shm_backend_send(xlink_channel_t* ch, const void* data, size_t len) {
    shm_priv_t* p = (shm_priv_t*)ch->priv;

    if (p->use_spsc) {
        int rc = spsc_enqueue_msg(p, data, len);
        if (rc != 0) {
            snprintf(ch->errbuf, sizeof(ch->errbuf),
                     "shm_spsc(%s): queue full", p->name);
            return -1;
        }
    } else {
        if (shm_writen(p->name, data, len) != 0) {
            snprintf(ch->errbuf, sizeof(ch->errbuf),
                     "shm_write(%s): %s", p->name, strerror(errno));
            return -1;
        }
    }

    /* Notify reader(s) via FIFO */
    fifo_notify(p->name);
    return 0;
}

static int shm_backend_recv(xlink_channel_t* ch, void* buf, size_t* len) {
    shm_priv_t* p = (shm_priv_t*)ch->priv;

    if (p->use_spsc) {
        size_t out_len = *len;
        int rc = spsc_dequeue_msg(p, buf, &out_len);
        if (rc > 0) {
            *len = out_len;
            return 0;
        }
        if (rc == 0) {
            snprintf(ch->errbuf, sizeof(ch->errbuf),
                     "shm_spsc(%s): no data", p->name);
            return -1;
        }
        snprintf(ch->errbuf, sizeof(ch->errbuf),
                 "shm_spsc(%s): error", p->name);
        return -1;
    }

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

static int shm_backend_read(xlink_channel_t* ch, void* buf, size_t len,
                            int timeout_ms) {
    shm_priv_t* p = (shm_priv_t*)ch->priv;
    struct timespec start, now;

    clock_gettime(CLOCK_MONOTONIC, &start);

    for (;;) {
        size_t avail = 0;
        if (shm_peek(p->name, NULL, &avail) == 0 && avail > 0) {
            size_t n = len;
            int rc = shm_read(p->name, buf, &n);
            if (rc == 0) return (int)n;
            snprintf(ch->errbuf, sizeof(ch->errbuf),
                     "shm_read(%s): %s", p->name, strerror(errno));
            return -1;
        }

        /* Check timeout */
        clock_gettime(CLOCK_MONOTONIC, &now);
        int64_t elapsed_ms = (now.tv_sec - start.tv_sec) * 1000
                           + (now.tv_nsec - start.tv_nsec) / 1000000;
        if (timeout_ms >= 0 && elapsed_ms >= timeout_ms) {
            errno = ETIMEDOUT;
            return -1;
        }

        usleep(500);  /* 500us poll interval */
    }
}

static int shm_backend_peek(xlink_channel_t* ch, size_t* avail) {
    shm_priv_t* p = (shm_priv_t*)ch->priv;

    if (p->use_spsc && p->spsc_hdr) {
        size_t head = atomic_load_explicit(&p->spsc_hdr->head, memory_order_acquire);
        size_t tail = atomic_load_explicit(&p->spsc_hdr->tail, memory_order_acquire);
        *avail = (head - tail);  /* bytes available in ring */
        return 0;
    }

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
    .read  = shm_backend_read,
    .peek  = shm_backend_peek,
};