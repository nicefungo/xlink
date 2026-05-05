/*
 * TCP backend — multi-client server with auto-reconnect client.
 *
 * Address format:
 *   "host:port"   → client mode: connect(host, port), auto-reconnect on loss
 *   ":port"       → server mode: listen, accept multiple clients
 *
 * Flags:
 *   XLINK_SERVER  → listen + multi-accept
 *   (default)     → connect to remote with auto-reconnect
 *
 * Stream transport: framing layer auto-enabled in xlink.c.
 */

#include "xlink_internal.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/uio.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/poll.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <arpa/inet.h>

/* ─── Private state ────────────────────────────────────── */

#define MAX_CLIENTS 64

typedef struct {
    /* Server mode */
    int   listen_fd;
    int   client_fds[MAX_CLIENTS];
    int   nclients;

    /* Client mode (reconnect info) */
    char*     recon_host;     /* saved for reconnection */
    uint16_t  recon_port;
    int       is_client;

    /* Reconnect backoff (client mode only) */
    int       recon_backoff;  /* current backoff ms, 0 = connected */
} tcp_priv_t;

/* ─── Helpers ──────────────────────────────────────────── */

static int tcp_set_nodelay(int fd) {
    int nodelay = 1;
    return setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
}

static void add_client(tcp_priv_t* p, int fd) {
    if (p->nclients >= MAX_CLIENTS) {
        close(fd);
        return;
    }
    tcp_set_nodelay(fd);
    p->client_fds[p->nclients++] = fd;
}

static void remove_client(tcp_priv_t* p, int idx) {
    if (idx < 0 || idx >= p->nclients) return;
    close(p->client_fds[idx]);
    p->client_fds[idx] = p->client_fds[--p->nclients];
}

/* Parse "host:port" or ":port" into host (malloc'd) and port */
static int parse_addr(const char* addr, char** host, uint16_t* port) {
    const char* colon = strrchr(addr, ':');
    if (!colon) return -1;

    size_t hostlen = (size_t)(colon - addr);
    *host = malloc(hostlen + 1);
    if (!*host) return -1;
    memcpy(*host, addr, hostlen);
    (*host)[hostlen] = '\0';

    /* Strip brackets from IPv6 addresses: [::1] → ::1 */
    size_t hl = strlen(*host);
    if (hl >= 2 && (*host)[0] == '[' && (*host)[hl - 1] == ']') {
        memmove(*host, *host + 1, hl - 2);
        (*host)[hl - 2] = '\0';
    }

    long p = atol(colon + 1);
    if (p < 1 || p > 65535) {
        free(*host);
        *host = NULL;
        return -1;
    }
    *port = (uint16_t)p;
    return 0;
}

/* Attempt a TCP connection to host:port. Returns fd or -1. */
static int tcp_try_connect(const char* host, uint16_t port) {
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%u", port);

    struct addrinfo* ai;
    if (getaddrinfo(host, port_str, &hints, &ai) != 0)
        return -1;

    int fd = -1;
    for (struct addrinfo* p = ai; p; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, p->ai_addr, p->ai_addrlen) == 0)
            break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(ai);
    return fd;
}

/* Apply O_NONBLOCK to ch->fd if XLINK_NONBLOCK flag is set */
static void apply_nonblock(xlink_channel_t* ch, int flags) {
    if ((flags & XLINK_NONBLOCK) && ch->fd >= 0) {
        int fl = fcntl(ch->fd, F_GETFL, 0);
        fcntl(ch->fd, F_SETFL, fl | O_NONBLOCK);
    }
}

/* ─── Open ──────────────────────────────────────────────── */

static int tcp_connect_client(xlink_channel_t* ch, const char* addr) {
    tcp_priv_t* p = calloc(1, sizeof(*p));
    if (!p) return -1;
    p->is_client = 1;

    int flags = ch->flags;

    if (parse_addr(addr, &p->recon_host, &p->recon_port) != 0) {
        snprintf(ch->errbuf, sizeof(ch->errbuf),
                 "tcp: invalid address '%s'", addr);
        free(p);
        return -1;
    }

    ch->fd = tcp_try_connect(p->recon_host, p->recon_port);
    if (ch->fd < 0) {
        snprintf(ch->errbuf, sizeof(ch->errbuf),
                 "tcp: connect(%s): %s", addr, strerror(errno));
        free(p->recon_host);
        free(p);
        return -1;
    }

    tcp_set_nodelay(ch->fd);
    apply_nonblock(ch, flags);
    ch->priv = p;
    ch->use_framing = 0;    /* backend manages framing for reconnect */
    return 0;
}

static int tcp_serve_multi(xlink_channel_t* ch, const char* addr,
                           const xlink_opt_t* opt) {
    char* host = NULL;
    uint16_t port = 0;

    if (parse_addr(addr, &host, &port) != 0) {
        snprintf(ch->errbuf, sizeof(ch->errbuf),
                 "tcp: invalid address '%s'", addr);
        return -1;
    }

    int fd = socket(AF_INET6, SOCK_STREAM, 0);
    if (fd < 0) {
        snprintf(ch->errbuf, sizeof(ch->errbuf),
                 "tcp: socket: %s", strerror(errno));
        free(host);
        return -1;
    }

    int no = 0;
    setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &no, sizeof(no));
    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in6 sin6;
    memset(&sin6, 0, sizeof(sin6));
    sin6.sin6_family = AF_INET6;
    sin6.sin6_port   = htons(port);
    sin6.sin6_addr   = in6addr_any;

    if (bind(fd, (struct sockaddr*)&sin6, sizeof(sin6)) < 0) {
        snprintf(ch->errbuf, sizeof(ch->errbuf),
                 "tcp: bind(%s): %s", addr, strerror(errno));
        close(fd);
        free(host);
        return -1;
    }

    int backlog = (opt && opt->tcp.backlog > 0) ? opt->tcp.backlog : 5;
    if (listen(fd, backlog) < 0) {
        snprintf(ch->errbuf, sizeof(ch->errbuf),
                 "tcp: listen: %s", strerror(errno));
        close(fd);
        free(host);
        return -1;
    }
    free(host);

    tcp_priv_t* p = calloc(1, sizeof(*p));
    if (!p) { close(fd); return -1; }
    p->listen_fd = fd;
    p->nclients  = 0;

    ch->fd = fd;   /* ch->fd = listen fd */
    ch->priv = p;
    ch->use_framing = 0;

    /* Non-blocking listen fd so multi-accept doesn't hang */
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);

    /* Apply NONBLOCK to listen fd if flag set */
    if (ch->flags & XLINK_NONBLOCK)
        apply_nonblock(ch, ch->flags);

    fprintf(stderr, "tcp: listening on %s\n", addr);
    return 0;
}

static int tcp_backend_open(xlink_channel_t* ch, const char* addr,
                            const xlink_opt_t* opt) {
    int flags = opt ? (int)opt->flags : 0;
    if (flags & XLINK_SERVER)
        return tcp_serve_multi(ch, addr, opt);
    else
        return tcp_connect_client(ch, addr);
}

/* ─── Close ─────────────────────────────────────────────── */

static void tcp_backend_close(xlink_channel_t* ch) {
    if (!ch->priv) {
        /* Legacy single-fd path (shouldn't happen) */
        if (ch->fd >= 0) { close(ch->fd); ch->fd = -1; }
        return;
    }

    tcp_priv_t* p = (tcp_priv_t*)ch->priv;

    if (p->is_client) {
        /* Client mode */
        if (ch->fd >= 0) { close(ch->fd); ch->fd = -1; }
        free(p->recon_host);
    } else {
        /* Server mode */
        if (p->listen_fd >= 0) close(p->listen_fd);
        for (int i = 0; i < p->nclients; i++)
            if (p->client_fds[i] >= 0) close(p->client_fds[i]);
        p->nclients = 0;
    }

    free(ch->priv);
    ch->priv = NULL;
    ch->fd = -1;
}

/* ─── Client reconnect ──────────────────────────────────── */

/*
 * Try to reconnect a disconnected client.
 * Uses exponential backoff: 100ms, 200ms, 400ms... up to 5000ms.
 * Returns 0 on success, -1 if still disconnected.
 */
static int try_reconnect(tcp_priv_t* p, xlink_channel_t* ch) {
    if (p->recon_backoff == 0)
        p->recon_backoff = 100;   /* start: 100ms */

    usleep((useconds_t)p->recon_backoff * 1000);

    int fd = tcp_try_connect(p->recon_host, p->recon_port);
    if (fd < 0) {
        /* Exponential backoff, cap at 5s */
        p->recon_backoff *= 2;
        if (p->recon_backoff > 5000)
            p->recon_backoff = 5000;
        return -1;
    }

    p->recon_backoff = 0;   /* connected, reset backoff */
    tcp_set_nodelay(fd);
    apply_nonblock(ch, ch->flags);
    /* We return the fd; caller stores it in ch->fd */
    return fd;
}

/* ─── Send (broadcast for server, single for client) ──── */

/* Read exactly n bytes from fd.
 *
 * EINTR: retry the read.
 * EAGAIN (non-blocking sockets):
 *   - If 0 bytes read so far → return -1 with errno == EAGAIN
 *   - If partial data consumed → poll() and retry (prevents framing desync
 *     when a message header is split across multiple read() calls)
 * Returns 0 on success, -1 on error (errno preserved). */
#include <time.h>

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + (int64_t)ts.tv_nsec / 1000000;
}

static int read_exact(int fd, void* buf, size_t n) {
    uint8_t* p = (uint8_t*)buf;
    size_t remaining = n;
    int64_t deadline = now_ms() + 30000;  /* 30s internal timeout */
    while (remaining > 0) {
        ssize_t nr;
        do { nr = read(fd, p, remaining); } while (nr < 0 && errno == EINTR);
        if (nr < 0) {
            if ((errno == EAGAIN || errno == EWOULDBLOCK) && p != (uint8_t*)buf) {
                /* Partial data consumed — poll for more instead of returning
                 * partial, which would desync the framer on the next call. */
                struct pollfd pfd = { .fd = fd, .events = POLLIN };
                int64_t now = now_ms();
                if (now >= deadline) {
                    errno = ETIMEDOUT;
                    return -1;
                }
                int poll_ms = (int)(deadline - now);
                if (poll_ms > 1000) poll_ms = 1000;
                int prc;
                do { prc = poll(&pfd, 1, poll_ms); } while (prc < 0 && errno == EINTR);
                if (prc == 0) continue;   /* poll timed out — recheck deadline */
                if (prc <= 0) return -1;
                continue;  /* retry the read */
            }
            return -1;  /* EAGAIN with no data, or real error */
        }
        if (nr == 0) return -1;  /* EOF */
        p += nr;
        remaining -= (size_t)nr;
    }
    return 0;
}

/* ─── Framing helpers (for client mode with reconnect) ───── */

/* Max retries for writev EAGAIN in non-blocking mode.
 * Each retry polls POLLOUT with 10ms timeout → ~1s of retry. */
#define MAX_WRITE_EAGAIN 100

/* Write 4-byte BE length prefix + payload via writev. */
static int write_framed(int fd, const void* data, size_t len) {
    uint8_t hdr[4];
    hdr[0] = (uint8_t)(len >> 24);
    hdr[1] = (uint8_t)(len >> 16);
    hdr[2] = (uint8_t)(len >> 8);
    hdr[3] = (uint8_t)(len);

    struct iovec iov[2];
    iov[0].iov_base = hdr;
    iov[0].iov_len  = 4;
    iov[1].iov_base = (void*)data;
    iov[1].iov_len  = len;

    int eagain_retries = 0;
    size_t total = 0;
    while (total < 4 + len) {
        ssize_t n = writev(fd, iov, 2);
        if (n < 0) {
            if (errno == EINTR) continue;
            if ((errno == EAGAIN || errno == EWOULDBLOCK)
                && eagain_retries < MAX_WRITE_EAGAIN) {
                struct pollfd pfd = { .fd = fd, .events = POLLOUT };
                int prc;
                do { prc = poll(&pfd, 1, 10); } while (prc < 0 && errno == EINTR);
                eagain_retries++;
                continue;
            }
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

/* Read 4-byte BE length prefix then payload.
 * *len IN = capacity, OUT = message size.
 * Returns 0 on success, -1 on error. */
static int read_framed(int fd, void* buf, size_t* len) {
    uint8_t hdr[4];
    if (read_exact(fd, hdr, 4) != 0) return -1;

    uint32_t msglen = (uint32_t)hdr[0] << 24
                    | (uint32_t)hdr[1] << 16
                    | (uint32_t)hdr[2] << 8
                    | (uint32_t)hdr[3];

    if (msglen > *len) {
        /* Discard all payload bytes to maintain framing sync,
         * then tell caller to try again with a larger buffer. */
        size_t remaining = msglen;
        while (remaining > 0) {
            uint8_t chunk[4096];
            size_t to_read = remaining > sizeof(chunk) ? sizeof(chunk) : remaining;
            if (read_exact(fd, chunk, to_read) != 0) return -1;
            remaining -= to_read;
        }
        errno = ENOSPC;
        return -1;
    }
    if (read_exact(fd, buf, msglen) != 0) return -1;
    *len = msglen;
    return 0;
}

static int send_to_all(tcp_priv_t* p, const void* data, size_t len,
                       xlink_channel_t* ch) {
    int ok_count = 0;
    for (int i = p->nclients - 1; i >= 0; i--) {
        int fd = p->client_fds[i];
        /*
         * write_framed uses a single writev(2) with retry for atomic
         * header+payload send.  Two separate writes (hdr then payload)
         * would risk a race: if the connection breaks between them, the
         * receiver gets a 4-byte header with no payload, causing framing
         * desync.  write_framed keeps header and payload in one stream op.
         */
        if (write_framed(fd, data, len) != 0) {
            /* Remove dead client */
            close(p->client_fds[i]);
            p->client_fds[i] = p->client_fds[--p->nclients];
        } else {
            ok_count++;
        }
    }
    if (ok_count == 0) {
        snprintf(ch->errbuf, sizeof(ch->errbuf),
                 "tcp: no connected clients");
        return -1;
    }
    return 0;
}

static int tcp_backend_send(xlink_channel_t* ch, const void* data, size_t len) {
    tcp_priv_t* p = (tcp_priv_t*)ch->priv;

    if (!p) {
        /* Fallback: single-fd path (shouldn't happen) */
        ssize_t n;
        do { n = write(ch->fd, data, len); } while (n < 0 && errno == EINTR);
        if (n < 0) {
            snprintf(ch->errbuf, sizeof(ch->errbuf),
                     "tcp write: %s", strerror(errno));
            return -1;
        }
        return 0;
    }

    if (!p->is_client) {
        /* Server mode: broadcast */
        return send_to_all(p, data, len, ch);
    }

    /* Client mode: send framed message with auto-reconnect */
    if (ch->fd < 0) {
        /* Disconnected — try reconnect */
        ch->fd = try_reconnect(p, ch);
        if (ch->fd < 0) {
            snprintf(ch->errbuf, sizeof(ch->errbuf),
                     "tcp: disconnected (reconnect in %dms)",
                     p->recon_backoff);
            return -1;
        }
    }

    if (write_framed(ch->fd, data, len) != 0) {
        /* Connection lost — mark for reconnect */
        int save_errno = errno;
        close(ch->fd);
        errno = save_errno;
        ch->fd = -1;
        if (p->recon_backoff == 0)
            p->recon_backoff = 100;
        snprintf(ch->errbuf, sizeof(ch->errbuf),
                 "tcp: lost connection: %s (will reconnect)",
                 strerror(errno));
        return -1;
    }
    return 0;
}

/* ─── Receive (multi-client for server, reconnect for client) ─── */

static int recv_multi(tcp_priv_t* p, xlink_channel_t* ch, void* buf, size_t* len) {
    for (;;) {
    struct pollfd fds[MAX_CLIENTS + 1];
    int nfds = 0;

    fds[nfds].fd = p->listen_fd;
    fds[nfds].events = POLLIN;
    nfds++;

    for (int i = 0; i < p->nclients; i++) {
        fds[nfds].fd = p->client_fds[i];
        fds[nfds].events = POLLIN;
        nfds++;
    }

    int rc;
    do {
        rc = poll(fds, (nfds_t)nfds, -1);
    } while (rc < 0 && errno == EINTR);

    if (rc < 0) {
        snprintf(ch->errbuf, sizeof(ch->errbuf),
                 "tcp: poll: %s", strerror(errno));
        return -1;
    }

    /* Accept new connections first */
    if (fds[0].revents & POLLIN) {
        for (;;) {
            int cfd = accept(p->listen_fd, NULL, NULL);
            if (cfd < 0) {
                if (errno == EINTR) continue;
                break;   /* EAGAIN/EWOULDBLOCK → no more pending */
            }
            tcp_set_nodelay(cfd);
            if (ch->flags & XLINK_NONBLOCK) {
                int cf = fcntl(cfd, F_GETFL, 0);
                fcntl(cfd, F_SETFL, cf | O_NONBLOCK);
            }
            add_client(p, cfd);
        }
    }

    /* Try to read a framed message from any ready client */
    for (int i = 1; i < nfds; i++) {
        if (!(fds[i].revents & (POLLIN | POLLHUP | POLLERR)))
            continue;

        int client_fd = fds[i].fd;

        /* Read framing header (4-byte BE length) */
        uint8_t hdr[4];
        if (read_exact(client_fd, hdr, 4) != 0) {
            remove_client(p, i - 1);
            goto next_client;
        }

        uint32_t msglen = (uint32_t)hdr[0] << 24
                        | (uint32_t)hdr[1] << 16
                        | (uint32_t)hdr[2] << 8
                        | (uint32_t)hdr[3];

        if (msglen > *len) {
            /* User buffer too small — discard all bytes to maintain framing sync.
             * Loop-read in case msglen exceeds the stack buffer size. */
            size_t remaining = msglen;
            while (remaining > 0) {
                uint8_t chunk[4096];
                size_t to_read = remaining > sizeof(chunk) ? sizeof(chunk) : remaining;
                if (read_exact(client_fd, chunk, to_read) != 0) {
                    remove_client(p, i - 1);
                    goto next_client;
                }
                remaining -= to_read;
            }
            continue;
        }

        /* Read payload */
        if (read_exact(client_fd, buf, msglen) != 0) {
            remove_client(p, i - 1);
            continue;
        }

        *len = msglen;
        return 0;

        next_client: ;
    }

    /* All ready clients were disconnected or exhausted; loop back */
    continue;
    }  /* for(;;) */
}  /* recv_multi */

static int tcp_backend_recv(xlink_channel_t* ch, void* buf, size_t* len) {
    tcp_priv_t* p = (tcp_priv_t*)ch->priv;

    if (!p) {
        /* Legacy single-fd path */
        ssize_t n;
        do { n = read(ch->fd, buf, *len); } while (n < 0 && errno == EINTR);
        if (n <= 0) {
            snprintf(ch->errbuf, sizeof(ch->errbuf),
                     "tcp read: %s", n == 0 ? "disconnected" : strerror(errno));
            return -1;
        }
        *len = (size_t)n;
        return 0;
    }

    if (p->is_client) {
        /* Client mode: read framed message with auto-reconnect */
        if (ch->fd < 0) {
            ch->fd = try_reconnect(p, ch);
            if (ch->fd < 0) {
                snprintf(ch->errbuf, sizeof(ch->errbuf),
                         "tcp: disconnected (reconnect in %dms)",
                         p->recon_backoff);
                return -1;
            }
        }

        if (read_framed(ch->fd, buf, len) == 0)
            return 0;

        /* Transient EAGAIN on non-blocking socket — don't disconnect */
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return -1;
        }

        /* Oversized message was discarded — try again next call,
         * no need to reconnect. */
        if (errno == ENOSPC) {
            return -1;
        }

        /* Disconnected */
        int save_errno = errno;
        close(ch->fd);
        errno = save_errno;
        ch->fd = -1;
        if (p->recon_backoff == 0) p->recon_backoff = 100;
        snprintf(ch->errbuf, sizeof(ch->errbuf),
                 "tcp: disconnected: %s (will reconnect)",
                 strerror(errno));
        return -1;
    }

    /* Server mode: multi-client recv */
    return recv_multi(p, ch, buf, len);
}

/* ─── Backend vtable ────────────────────────────────────── */

const xlink_backend_t xlink_tcp_backend = {
    .type  = XLINK_TCP,
    .name  = "tcp",
    .open  = tcp_backend_open,
    .close = tcp_backend_close,
    .send  = tcp_backend_send,
    .recv  = tcp_backend_recv,
    .write = NULL,
    .read  = NULL,
    .peek  = NULL,
};
