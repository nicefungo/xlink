/*
 * tls.c — OpenSSL TLS wrapper for xlink TCP channels
 *
 * Provides TLS encryption transparently on top of TCP sockets.
 * Compile with -DXLINK_HAS_TLS and link -lssl -lcrypto.
 *
 * API:
 *   xlink_tls_configure(ch, cfg) → handshake on first send/recv
 *   xlink_tls_enabled(ch)        → query
 *
 * Internal hooks (called by tcp_backend.c):
 *   tls_wrap(), tls_unwrap(), tls_read(), tls_write()
 */

#include "xlink_internal.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#ifdef XLINK_HAS_TLS

#include <openssl/ssl.h>
#include <openssl/err.h>

/* ─── Per-channel TLS state ───────────────────────────── */

typedef struct {
    SSL_CTX *ctx;
    SSL     *ssl;
    int      is_server;   /* 1 = server (SSL_accept), 0 = client (SSL_connect) */
    int      handshake_done;
    int      configured;
} tls_state_t;

/* ─── Global init / cleanup ───────────────────────────── */

static int g_tls_init_done = 0;

static void tls_global_init(void) {
    if (g_tls_init_done) return;
    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();
    g_tls_init_done = 1;
}

/* ─── SSL_CTX creation ────────────────────────────────── */

static SSL_CTX *tls_create_ctx(const xlink_tls_config_t *cfg, int is_server) {
    const SSL_METHOD *method;
    SSL_CTX *ctx;

    if (is_server)
        method = TLS_server_method();
    else
        method = TLS_client_method();

    ctx = SSL_CTX_new(method);
    if (!ctx) return NULL;

    /* Disable old protocols — TLS 1.2+ only */
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);

    /* Server: load cert + key */
    if (is_server && cfg->cert_file && cfg->key_file) {
        if (SSL_CTX_use_certificate_file(ctx, cfg->cert_file,
                                          SSL_FILETYPE_PEM) != 1)
            goto fail;
        if (SSL_CTX_use_PrivateKey_file(ctx, cfg->key_file,
                                         SSL_FILETYPE_PEM) != 1)
            goto fail;
        if (SSL_CTX_check_private_key(ctx) != 1)
            goto fail;
    }

    /* CA verification */
    if (cfg->verify_peer) {
        if (cfg->ca_file) {
            if (SSL_CTX_load_verify_locations(ctx, cfg->ca_file, NULL) != 1)
                goto fail;
        } else {
            /* Use system default CA store */
            SSL_CTX_set_default_verify_paths(ctx);
        }
        SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
    } else {
        SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);
    }

    return ctx;

fail:
    SSL_CTX_free(ctx);
    return NULL;
}

/* ─── Public API: configure ───────────────────────────── */

int xlink_tls_configure(xlink_channel_t *ch,
                        const xlink_tls_config_t *cfg) {
    if (!ch || !cfg) return -1;
    if (ch->backend && ch->backend->type != XLINK_TCP) {
        snprintf(ch->errbuf, sizeof(ch->errbuf),
                 "TLS only supported on TCP channels");
        return -1;
    }

    tls_global_init();

    tls_state_t *ts = calloc(1, sizeof(*ts));
    if (!ts) return -1;

    int is_server = (ch->opt.flags & XLINK_SERVER) != 0;
    ts->ctx = tls_create_ctx(cfg, is_server);
    if (!ts->ctx) {
        snprintf(ch->errbuf, sizeof(ch->errbuf),
                 "TLS: failed to create SSL_CTX: %s",
                 ERR_error_string(ERR_get_error(), NULL));
        free(ts);
        return -1;
    }

    ts->ssl = SSL_new(ts->ctx);
    if (!ts->ssl) {
        snprintf(ch->errbuf, sizeof(ch->errbuf),
                 "TLS: SSL_new failed: %s",
                 ERR_error_string(ERR_get_error(), NULL));
        SSL_CTX_free(ts->ctx);
        free(ts);
        return -1;
    }

    ts->is_server = is_server;
    ts->configured = 1;

    /* SNI (client mode) */
    if (!is_server && cfg->sni_hostname)
        SSL_set_tlsext_host_name(ts->ssl, cfg->sni_hostname);

    ch->tls = ts;
    return 0;
}

int xlink_tls_enabled(xlink_channel_t *ch) {
    return (ch && ch->tls && ((tls_state_t *)ch->tls)->configured);
}

/* ─── Internal: handshake ─────────────────────────────── */

static int tls_do_handshake(xlink_channel_t *ch, tls_state_t *ts) {
    if (ts->handshake_done) return 0;

    /* Bind SSL to socket fd for transparent I/O */
    if (!SSL_set_fd(ts->ssl, ch->fd)) {
        snprintf(ch->errbuf, sizeof(ch->errbuf),
                 "TLS: SSL_set_fd failed");
        return -1;
    }

    int ret;
    if (ts->is_server)
        ret = SSL_accept(ts->ssl);
    else
        ret = SSL_connect(ts->ssl);

    if (ret != 1) {
        int ssl_err = SSL_get_error(ts->ssl, ret);
        const char *detail = "";
        if (ssl_err == SSL_ERROR_SSL)
            detail = ERR_error_string(ERR_get_error(), NULL);
        else if (ssl_err == SSL_ERROR_SYSCALL)
            detail = strerror(errno);
        snprintf(ch->errbuf, sizeof(ch->errbuf),
                 "TLS handshake failed: %s (ssl_err=%d)",
                 detail, ssl_err);
        return -1;
    }

    ts->handshake_done = 1;
    return 0;
}

/* ─── Internal: TLS I/O (called by tcp_backend.c) ─────── */

/* tls_write: write data through TLS to socket.
 * Returns 0 on success, -1 on error. */
int xlink_tls_write(xlink_channel_t *ch, const void *data, size_t len) {
    tls_state_t *ts = (tls_state_t *)ch->tls;
    if (!ts || !ts->configured) {
        snprintf(ch->errbuf, sizeof(ch->errbuf),
                 "TLS: channel not configured");
        return -1;
    }

    if (tls_do_handshake(ch, ts) != 0) return -1;

    size_t total = 0;
    while (total < len) {
        int n = SSL_write(ts->ssl, (const char *)data + total,
                          (int)(len - total));
        if (n <= 0) {
            snprintf(ch->errbuf, sizeof(ch->errbuf),
                     "TLS write error: %s",
                     ERR_error_string(ERR_get_error(), NULL));
            return -1;
        }
        total += (size_t)n;
    }
    return 0;
}

/* tls_read: read data from TLS socket.
 * On success, *len is set to bytes read, returns 0.
 * Returns -1 on error. */
int xlink_tls_read(xlink_channel_t *ch, void *buf, size_t *len) {
    tls_state_t *ts = (tls_state_t *)ch->tls;
    if (!ts || !ts->configured) {
        snprintf(ch->errbuf, sizeof(ch->errbuf),
                 "TLS: channel not configured");
        return -1;
    }

    if (tls_do_handshake(ch, ts) != 0) return -1;

    size_t request = *len;
    size_t total = 0;
    while (total < request) {
        int n = SSL_read(ts->ssl, (char *)buf + total, (int)(request - total));
        if (n <= 0) {
            int ssl_err = SSL_get_error(ts->ssl, n);
            if (ssl_err == SSL_ERROR_ZERO_RETURN) {
                *len = total;
                return (total == 0) ? -1 : 0;
            }
            if (total > 0) {
                /* Partial read — return what we have */
                *len = total;
                return 0;
            }
            snprintf(ch->errbuf, sizeof(ch->errbuf),
                     "TLS read error: %s",
                     ERR_error_string(ERR_get_error(), NULL));
            return -1;
        }
        total += (size_t)n;
    }
    *len = total;
    return 0;
}

/* ─── Cleanup ─────────────────────────────────────────── */

void xlink_tls_cleanup(xlink_channel_t *ch) {
    if (!ch || !ch->tls) return;
    tls_state_t *ts = (tls_state_t *)ch->tls;

    if (ts->ssl) {
        SSL_shutdown(ts->ssl);
        SSL_free(ts->ssl);
    }
    if (ts->ctx)
        SSL_CTX_free(ts->ctx);

    free(ts);
    ch->tls = NULL;
}

/* ─── Per-client TLS (server mode) ────────────────────── */

/* Clone a new SSL object for a client from the channel's SSL_CTX.
 * The ctx is shared; only the SSL is new.  Returns a tls_state_t*
 * with ctx=NULL (borrowed from ch->tls) and a fresh SSL bound to fd.
 * Handshake happens on first I/O via the normal tls_do_handshake path. */
void *tls_clone_for_client(void *ch_tls, int client_fd, xlink_channel_t *ch) {
    tls_state_t *ts = (tls_state_t *)ch_tls;
    if (!ts || !ts->ctx) return NULL;

    tls_state_t *cs = calloc(1, sizeof(*cs));
    if (!cs) return NULL;

    cs->ssl = SSL_new(ts->ctx);
    if (!cs->ssl) {
        snprintf(ch->errbuf, sizeof(ch->errbuf),
                 "TLS: SSL_new for client failed: %s",
                 ERR_error_string(ERR_get_error(), NULL));
        free(cs);
        return NULL;
    }

    SSL_set_fd(cs->ssl, client_fd);
    cs->is_server = 1;
    cs->configured = 1;
    cs->handshake_done = 0;  /* do handshake on first I/O */
    cs->ctx = NULL;  /* borrowed from channel, don't free */

    return cs;
}

/* Free per-client SSL (but NOT the CTX — that's shared and freed by channel). */
void xlink_tls_free_client_ssl(void *tls) {
    if (!tls) return;
    tls_state_t *cs = (tls_state_t *)tls;
    if (cs->ssl) {
        SSL_shutdown(cs->ssl);
        SSL_free(cs->ssl);
    }
    /* cs->ctx is borrowed — do NOT free */
    free(cs);
}

#else /* !XLINK_HAS_TLS — stubs */

int xlink_tls_configure(xlink_channel_t *ch,
                        const xlink_tls_config_t *cfg) {
    (void)cfg;
    snprintf(ch->errbuf, sizeof(ch->errbuf),
             "TLS not compiled in (build with XLINK_HAS_TLS)");
    return -1;
}

int xlink_tls_enabled(xlink_channel_t *ch) {
    (void)ch;
    return 0;
}

int xlink_tls_write(xlink_channel_t *ch, const void *data, size_t len) {
    (void)ch; (void)data; (void)len;
    return -1;
}

int xlink_tls_read(xlink_channel_t *ch, void *buf, size_t *len) {
    (void)ch; (void)buf; (void)len;
    return -1;
}

void xlink_tls_cleanup(xlink_channel_t *ch) {
    (void)ch;
}

#endif /* XLINK_HAS_TLS */