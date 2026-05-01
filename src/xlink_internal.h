#ifndef XLINK_INTERNAL_H
#define XLINK_INTERNAL_H

#include "xlink.h"
#include <stdint.h>
#include <stddef.h>

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

/* ─── Channel struct (exposed to backends) ────────────── */
struct xlink_channel {
    const xlink_backend_t* backend;
    void*                  priv;
    int                    fd;
    int                    flags;
    int                    use_framing;
    char                   errbuf[128];
};

/* ─── Backend declarations (defined in backend .c files) ─ */
extern const xlink_backend_t xlink_shm_backend;
extern const xlink_backend_t xlink_pipe_backend;
extern const xlink_backend_t xlink_tcp_backend;
extern const xlink_backend_t xlink_udp_backend;
extern const xlink_backend_t xlink_file_backend;
extern const xlink_backend_t xlink_serial_backend;

/* ─── Helpers usable by backends ───────────────────────── */

/* Register a SHM name for atexit cleanup.
 * Called by shm backend when XLINK_CREATE is used. */
void xlink_register_shm_cleanup(const char* name);

#endif /* XLINK_INTERNAL_H */
