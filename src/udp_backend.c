#include "xlink_internal.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>

/*
 * UDP backend — datagram-oriented, no framing.
 *
 * Address formats:
 *   ":port"          → RECEIVER: bind to port, wait for datagrams
 *   "host:port"      → SENDER:   no bind, send to host:port with ephemeral src
 *   "group:port"     → SENDER to multicast group
 *
 * XLINK_CREATE flag: force bind even for "host:port" addresses
 */

typedef struct {
    struct sockaddr_storage dest_addr;
    socklen_t              dest_len;
    int                    is_receiver;
} udp_priv_t;

/* IPv4-mapped IPv6 socket */
static int udp_socket(void) {
    int fd = socket(AF_INET6, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    int no = 0;
    setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &no, sizeof(no));
    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    return fd;
}

static int udp_backend_open(xlink_channel_t* ch, const char* addr,
                            const xlink_opt_t* opt) {
    int flags = opt ? (int)opt->flags : 0;

    /* Split address at last ':' */
    const char* colon = strrchr(addr, ':');
    if (!colon) {
        snprintf(ch->errbuf, sizeof(ch->errbuf),
                 "udp: invalid address '%s' (need :port)", addr);
        return -1;
    }

    size_t hostlen = (size_t)(colon - addr);
    long port = atol(colon + 1);
    if (port < 1 || port > 65535) {
        snprintf(ch->errbuf, sizeof(ch->errbuf),
                 "udp: invalid port in '%s'", addr);
        return -1;
    }

    udp_priv_t* p = calloc(1, sizeof(*p));
    if (!p) return -1;

    /* Is this a receiver (":port") or sender ("host:port")? */
    p->is_receiver = (hostlen == 0) || (flags & XLINK_CREATE);

    if (p->is_receiver) {
        /* ── Receiver mode: bind to port ── */
        ch->fd = udp_socket();
        if (ch->fd < 0) {
            snprintf(ch->errbuf, sizeof(ch->errbuf),
                     "udp: socket: %s", strerror(errno));
            free(p);
            return -1;
        }

        struct sockaddr_in6 sin6;
        memset(&sin6, 0, sizeof(sin6));
        sin6.sin6_family = AF_INET6;
        sin6.sin6_port   = htons((uint16_t)port);
        sin6.sin6_addr   = in6addr_any;

        if (bind(ch->fd, (struct sockaddr*)&sin6, sizeof(sin6)) < 0) {
            snprintf(ch->errbuf, sizeof(ch->errbuf),
                     "udp: bind(:%ld): %s", port, strerror(errno));
            free(p);
            close(ch->fd);
            ch->fd = -1;
            return -1;
        }

        /* If a host was given, try joining multicast group */
        if (hostlen > 0) {
            char host[256];
            if (hostlen < sizeof(host)) {
                memcpy(host, addr, hostlen);
                host[hostlen] = '\0';

                struct in6_addr mcast_addr;
                if (inet_pton(AF_INET6, host, &mcast_addr) == 1
                    && IN6_IS_ADDR_MULTICAST(&mcast_addr)) {
                    struct ipv6_mreq mreq;
                    memcpy(&mreq.ipv6mr_multiaddr, &mcast_addr,
                           sizeof(mreq.ipv6mr_multiaddr));
                    mreq.ipv6mr_interface = 0;
                    setsockopt(ch->fd, IPPROTO_IPV6, IPV6_JOIN_GROUP,
                               &mreq, sizeof(mreq));
                }
            }
        }
    } else {
        /* ── Sender mode: resolve destination, use ephemeral port ── */
        ch->fd = udp_socket();
        if (ch->fd < 0) {
            snprintf(ch->errbuf, sizeof(ch->errbuf),
                     "udp: socket: %s", strerror(errno));
            free(p);
            return -1;
        }

        /* Build destination address */
        char host[256];
        if (hostlen >= sizeof(host)) {
            close(ch->fd);
            ch->fd = -1;
            free(p);
            snprintf(ch->errbuf, sizeof(ch->errbuf),
                     "udp: host too long in '%s'", addr);
            return -1;
        }
        memcpy(host, addr, hostlen);
        host[hostlen] = '\0';

        struct sockaddr_in6* sin6 = (struct sockaddr_in6*)&p->dest_addr;
        memset(sin6, 0, sizeof(*sin6));
        sin6->sin6_family = AF_INET6;
        sin6->sin6_port   = htons((uint16_t)port);

        /* Try IPv6 first, then IPv4-mapped, then DNS */
        if (inet_pton(AF_INET6, host, &sin6->sin6_addr) != 1) {
            struct in_addr v4;
            if (inet_pton(AF_INET, host, &v4) == 1) {
                memset(&sin6->sin6_addr, 0, 16);
                sin6->sin6_addr.s6_addr[10] = 0xff;
                sin6->sin6_addr.s6_addr[11] = 0xff;
                memcpy(&sin6->sin6_addr.s6_addr[12], &v4, 4);
            } else {
                /* DNS resolution */
                struct addrinfo hints;
                memset(&hints, 0, sizeof(hints));
                hints.ai_family   = AF_INET6;
                hints.ai_socktype = SOCK_DGRAM;
                hints.ai_flags    = AI_V4MAPPED;

                char port_str[16];
                snprintf(port_str, sizeof(port_str), "%ld", port);

                struct addrinfo* ai;
                if (getaddrinfo(host, port_str, &hints, &ai) != 0) {
                    close(ch->fd);
                    ch->fd = -1;
                    free(p);
                    snprintf(ch->errbuf, sizeof(ch->errbuf),
                             "udp: cannot resolve '%.80s'", host);
                    return -1;
                }
                memcpy(sin6, ai->ai_addr,
                       ai->ai_addrlen < (int)sizeof(*sin6)
                       ? (size_t)ai->ai_addrlen : sizeof(*sin6));
                freeaddrinfo(ai);
            }
        }
        p->dest_len = sizeof(*sin6);
    }

    ch->priv = p;
    ch->use_framing = 0;

    if (flags & XLINK_NONBLOCK) {
        int fl = fcntl(ch->fd, F_GETFL, 0);
        fcntl(ch->fd, F_SETFL, fl | O_NONBLOCK);
    }

    return 0;
}

static void udp_backend_close(xlink_channel_t* ch) {
    if (ch->fd >= 0) {
        close(ch->fd);
        ch->fd = -1;
    }
    free(ch->priv);
    ch->priv = NULL;
}

static int udp_backend_send(xlink_channel_t* ch, const void* data, size_t len) {
    udp_priv_t* p = (udp_priv_t*)ch->priv;

    if (p && p->dest_len > 0) {
        ssize_t n;
        do {
            n = sendto(ch->fd, data, len, 0,
                       (struct sockaddr*)&p->dest_addr, p->dest_len);
        } while (n < 0 && errno == EINTR);
        if (n < 0) {
            snprintf(ch->errbuf, sizeof(ch->errbuf),
                     "udp sendto: %s", strerror(errno));
            return -1;
        }
    } else if (p && p->is_receiver) {
        /* Receiver sending to default route (loopback) */
        ssize_t n;
        do { n = write(ch->fd, data, len); } while (n < 0 && errno == EINTR);
        if (n < 0) {
            snprintf(ch->errbuf, sizeof(ch->errbuf),
                     "udp write: %s", strerror(errno));
            return -1;
        }
    } else {
        snprintf(ch->errbuf, sizeof(ch->errbuf),
                 "udp send: no destination address");
        return -1;
    }
    return 0;
}

static int udp_backend_recv(xlink_channel_t* ch, void* buf, size_t* len) {
    ssize_t n;
    do { n = recvfrom(ch->fd, buf, *len, 0, NULL, NULL); }
    while (n < 0 && errno == EINTR);
    if (n < 0) {
        snprintf(ch->errbuf, sizeof(ch->errbuf),
                 "udp recvfrom: %s", strerror(errno));
        return -1;
    }
    *len = (size_t)n;
    return 0;
}

const xlink_backend_t xlink_udp_backend = {
    .type  = XLINK_UDP,
    .name  = "udp",
    .open  = udp_backend_open,
    .close = udp_backend_close,
    .send  = udp_backend_send,
    .recv  = udp_backend_recv,
    .write = NULL,
    .read  = NULL,
    .peek  = NULL,
};
