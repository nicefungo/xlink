#ifndef XLINK_H
#define XLINK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ═══════════════════════════════════════════════════════════
 * xlink — Cross-App Communication Toolkit
 *
 * One API for all transports:
 *   SHM  |  Pipe  |  TCP  |  UDP  |  Serial  |  RTSP  |  File
 *
 *   xlink_open()   → get a channel
 *   xlink_send()   → send a framed message
 *   xlink_recv()   → receive a framed message
 *   xlink_close()  → done
 * ═══════════════════════════════════════════════════════════ */

/* ─── Transport Types ─────────────────────────────────── */

typedef enum {
    XLINK_SHM,      /* POSIX shared memory (shm_open + mmap)       */
    XLINK_PIPE,     /* Named pipe (FIFO)                           */
    XLINK_TCP,      /* TCP stream (client / server)                */
    XLINK_UDP,      /* UDP datagram (unicast / multicast)          */
    XLINK_SERIAL,   /* RS-232 / RS-485 serial port                 */
    XLINK_RTSP,     /* RTSP client (pull video/audio stream)       */
    XLINK_FILE,     /* File I/O (dump / replay)                    */
} xlink_type_t;

/* ─── Open Flags ──────────────────────────────────────── */

typedef enum {
    XLINK_CREATE   = 1 << 0,   /* Create endpoint if absent (SHM, PIPE) */
    XLINK_SERVER   = 1 << 1,   /* Bind / listen (TCP, PIPE)              */
    XLINK_NONBLOCK = 1 << 2,   /* Non-blocking I/O                       */
    XLINK_BROADCAST = 1 << 3,  /* Multiple consumers (SHM)              */
} xlink_flag_t;

/* ─── Open Options ────────────────────────────────────── */

typedef struct {
    uint32_t     flags;        /* bitmask of xlink_flag_t   */
    size_t       buf_size;     /* buffer size (SHM, internal) */
    int          timeout_ms;   /* recv timeout, -1 = block   */
    union {
        struct { int mode;              } shm;    /* 0=compete,1=broadcast */
        struct { int backlog;           } tcp;    /* listen() backlog       */
        struct { int baud; int bits; int parity; int stop; } serial;
        struct { const char* iface; int ttl;     } mcast;
    };
} xlink_opt_t;

/* Default options — call before customizing.
 * Macro expands to a C99 compound literal, usable in both
 * declaration (xlink_opt_t opt = XLINK_OPT_DEFAULT;) and
 * assignment (opt = XLINK_OPT_DEFAULT;) contexts.
 * Do NOT use for static-duration initializers (C89 restriction). */
#define XLINK_OPT_DEFAULT \
    (xlink_opt_t){ .flags = 0, .buf_size = 0, .timeout_ms = -1, .shm = {0} }

/* ─── Channel Handle (opaque) ─────────────────────────── */

typedef struct xlink_channel xlink_channel_t;

/* ─── Core API ────────────────────────────────────────── */

/* Open a channel.
 *
 * addr format by type:
 *   XLINK_SHM    "name"             — POSIX shm name (/dev/shm/...)
 *   XLINK_PIPE   "/path/to/fifo"    — FIFO filesystem path
 *   XLINK_TCP    "host:port"        — connect as client
 *                ":port"            — bind & listen as server
 *   XLINK_UDP    "host:port"        — unicast
 *                "group:port"       — multicast group address
 *   XLINK_SERIAL "/dev/ttyX:baud"   — e.g. "/dev/ttyUSB0:115200"
 *   XLINK_RTSP   "rtsp://..."       — RTSP stream URL
 *   XLINK_FILE   "/path/to/file"    — for dump / replay
 *
 * Returns NULL on error (check xlink_errstr).
 */
xlink_channel_t* xlink_open(xlink_type_t type, const char* addr,
                            const xlink_opt_t* opt);

/* Open a channel by URL — auto-detect protocol from scheme.
 *
 *   xlink_open_url("shm://mychan", NULL)
 *   xlink_open_url("tcp://server:8080", &opt)
 *   xlink_open_url("pipe:///tmp/myfifo", opt_with_create)
 *
 * Returns NULL if scheme unknown (check xlink_errstr).
 */
xlink_channel_t* xlink_open_url(const char* url,
                                const xlink_opt_t* opt);

/* ─── Plugin System ───────────────────────────────────── */

/* Load a plugin from a shared library (.so).
 * The .so must export a symbol named "xlink_plugin_export"
 * of type xlink_plugin_t.
 * Returns 0 on success, -1 on error.
 */
int xlink_plugin_load(const char *so_path);

/* Return the number of registered plugins (including built-ins). */
size_t xlink_plugin_count(void);

/* ─── Data Transfer ───────────────────────────────────── */

/* Send a framed message (length prefix + payload).
 * Returns 0 on success, -1 on error.
 */
int xlink_send(xlink_channel_t* ch, const void* data, size_t len);

/* Receive a framed message.
 *   *len  = capacity of buf on entry, actual size on return.
 * Returns 0 on success, -1 on error / timeout.
 */
int xlink_recv(xlink_channel_t* ch, void* buf, size_t* len);

/* Low-level stream write/read (bypasses framing layer).
 * For transports that support streaming semantics.
 */
int xlink_write(xlink_channel_t* ch, const void* data, size_t len);
int xlink_read(xlink_channel_t* ch, void* buf, size_t len, int timeout_ms);

/* Peek at available data without consuming.
 * Sets *avail to number of bytes available.
 * Returns 0 on success, -1 if not supported by backend.
 */
int xlink_peek(xlink_channel_t* ch, size_t* avail);

/* Close channel and free all resources. */
void xlink_close(xlink_channel_t* ch);

/* ─── Multi-Channel Wait ──────────────────────────────── */

/* Wait for data on any of n channels.
 *
 * Blocks until at least one channel has data ready, or timeout_ms expires.
 *   chans[]   — array of channel pointers (non-NULL expected)
 *   n         — number of channels
 *   timeout_ms — max wait; -1 = infinite, 0 = poll once
 *
 * Returns the index (0..n-1) of the first ready channel,
 *   -1 on timeout, -2 on error.
 */
int xlink_wait(xlink_channel_t** chans, int n, int timeout_ms);

/* Event-driven multi-channel wait using async engine.
 *
 * Like xlink_wait() but uses the given async engine (epoll/io_uring)
 * instead of poll()+usleep().  Better performance under high concurrency.
 * Pass NULL for aio to get default engine (auto-select).
 *
 * Returns channel index (0..n-1), -1 on timeout, -2 on error.
 */
int xlink_wait_aio(xlink_channel_t** chans, int n,
                   int timeout_ms, void *aio_engine);

/* ─── Async I/O Engine ────────────────────────────────── */

/* Create an async engine.
 * type: 0 = AUTO, 1 = POLL, 2 = EPOLL, 3 = IO_URING.
 * Use 0 for automatic selection based on platform. */
void *xlink_aio_create(int type);

/* Destroy an async engine. */
void  xlink_aio_destroy(void *engine);

/* Get engine name ("poll", "epoll", "io_uring"). */
const char *xlink_aio_name(void *engine);

/* ─── Error Reporting ─────────────────────────────────── */

/* Return human-readable string for last error on this channel. */
const char* xlink_errstr(xlink_channel_t* ch);

/* Return transport type string ("shm", "tcp", ...). */
const char* xlink_type_str(xlink_type_t t);

/* Dump channel info to file descriptor (e.g. stderr). */
void xlink_dump(xlink_channel_t* ch, int fd);

#ifdef __cplusplus
}
#endif

#endif /* XLINK_H */
