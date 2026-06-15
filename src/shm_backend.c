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

typedef struct {
    char   name[64];
    int    fifo_created;   /* did we create the FIFO? (only on CREATE) */
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

static int shm_backend_send(xlink_channel_t* ch, const void* data, size_t len) {
    shm_priv_t* p = (shm_priv_t*)ch->priv;
    if (shm_writen(p->name, data, len) != 0) {
        snprintf(ch->errbuf, sizeof(ch->errbuf),
                 "shm_write(%s): %s", p->name, strerror(errno));
        return -1;
    }
    /* Notify reader(s) via FIFO */
    fifo_notify(p->name);
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