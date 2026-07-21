#ifndef XLINK_INTERNAL_H
#define XLINK_INTERNAL_H

#include "xlink.h"
#include "spsc_queue.h"
#include <stdint.h>
#include <stddef.h>
#include <time.h>

/* ─── Backend operations table ────────────────────────── */
typedef struct {
    xlink_type_t type;
    const char*  name;
    int  (*open) (xlink_channel_t* ch, const char* addr, const xlink_opt_t* opt);
    void (*close)(xlink_channel_t* ch);
    int  (*send) (xlink_channel_t* ch, const void* data, size_t len);
    int  (*recv) (xlink_channel_t* ch, void* buf, size_t* len);
    int  (*write)(xlink_channel_t* ch, const void* data, size_t len);
    int  (*read) (xlink_channel_t* ch, void* buf, size_t len, int timeout_ms);
    int  (*peek) (xlink_channel_t* ch, size_t* avail);
} xlink_backend_t;

/* ─── Adaptive batch state (internal) ─────────────────── */

#define XLINK_BATCH_QCAP   128    /* fixed-size ring buffer for batch queue */

struct xlink_batch_state {
    /* queued messages ring buffer */
    xlink_msg_t  queue[XLINK_BATCH_QCAP];
    int          q_head;           /* producer index */
    int          q_tail;           /* consumer index */
    int          q_count;          /* number of queued messages */

    /* policy */
    xlink_batch_policy_t policy;

    /* EWMA rate tracking */
    double       avg_msg_rate;     /* EWMA: messages per second */
    double       avg_msg_size;     /* EWMA: bytes per message */
    int          samples;          /* number of EWMA updates */
    int          current_batch;    /* current dynamic batch size */

    /* timing */
    struct timespec first_queued;  /* when first msg of batch was queued */
    struct timespec last_flush;    /* last flush monotonic time */
};

/* ─── Channel struct (exposed to backends) ────────────── */
struct xlink_channel {
    const xlink_backend_t* backend;
    void*                  priv;
    int                    fd;
    int                    flags;
    int                    use_framing;
    char                   errbuf[128];
    xlink_opt_t            opt;      /* saved open options */
    void                  *tls;      /* TLS state (tls_state_t *) */
    struct xlink_batch_state *bs;    /* adaptive batching state (NULL if disabled) */
    void                  *lfq;     /* lock-free SPSC send queue (NULL if unused) */
};

/* ─── Backend declarations (defined in backend .c files) ─ */
extern const xlink_backend_t xlink_shm_backend;
extern const xlink_backend_t xlink_pipe_backend;
extern const xlink_backend_t xlink_tcp_backend;
extern const xlink_backend_t xlink_udp_backend;
extern const xlink_backend_t xlink_file_backend;
extern const xlink_backend_t xlink_serial_backend;

/* ─── Plugin system ───────────────────────────────────── */

/* Plugin API version — bump when xlink_plugin_t layout changes */
#define XLINK_PLUGIN_API_VERSION  1

typedef struct xlink_plugin {
    const char        *name;           /* protocol name, e.g. "mqtt"     */
    const char        *version;        /* plugin version, e.g. "1.0.0"   */
    int                api_version;    /* XLINK_PLUGIN_API_VERSION        */
    xlink_type_t       proto;          /* protocol type ID               */
    const xlink_backend_t *backend;    /* the backend vtable              */

    /* lifecycle: called once at load / unload */
    int  (*init)(void);
    void (*fini)(void);

    void *_reserved[4];
} xlink_plugin_t;

/* Plugin manager API */
int  xlink_plugin_register(const xlink_plugin_t *plugin);
int  xlink_plugin_unregister(const char *name);
const xlink_plugin_t *xlink_plugin_find(const char *name);
const xlink_plugin_t *xlink_plugin_find_by_type(xlink_type_t type);
int  xlink_plugin_load(const char *so_path);
void xlink_plugins_init(void);
size_t xlink_plugin_count_impl(void);

/* ─── TLS internal hooks ──────────────────────────────── */

#ifdef XLINK_HAS_TLS
int  xlink_tls_write(xlink_channel_t *ch, const void *data, size_t len);
int  xlink_tls_read(xlink_channel_t *ch, void *buf, size_t *len);
void xlink_tls_cleanup(xlink_channel_t *ch);
#endif

/* ─── Helpers usable by backends ───────────────────────── */

/* Register a SHM name for atexit cleanup.
 * Called by shm backend when XLINK_CREATE is used. */
void xlink_register_shm_cleanup(const char* name);

#endif /* XLINK_INTERNAL_H */
