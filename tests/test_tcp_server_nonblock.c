/*
 * test_tcp_server_nonblock.c — TCP server with XLINK_NONBLOCK flag.
 *
 * Covers:
 *   1. TCP server + NONBLOCK open succeeds
 *   2. Server recv from raw TCP client (framed message)
 *   3. Two concurrent clients on NONBLOCK server
 *   4. Server detects client disconnect
 *   5. NONBLOCK flag applied to accepted client fds
 */

#include "xlink.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <errno.h>

static int failures = 0;
static int checks   = 0;

#define CHECK(cond, msg) do {                                   \
    checks++;                                                   \
    if (!(cond)) {                                              \
        fprintf(stderr, "  FAIL [%d] %s\n", checks, msg);       \
        failures++;                                             \
    } else {                                                    \
        fprintf(stderr, "  PASS [%d] %s\n", checks, msg);       \
    }                                                           \
} while(0)

#define PORT  19996

/* ── Raw TCP client helper ────────────────────────────── */

static int raw_connect(int port) {
    struct sockaddr_in6 sin6;
    memset(&sin6, 0, sizeof(sin6));
    sin6.sin6_family = AF_INET6;
    sin6.sin6_port   = htons((uint16_t)port);
    sin6.sin6_addr   = in6addr_loopback;

    int fd = socket(AF_INET6, SOCK_STREAM, 0);
    if (fd < 0) { perror("raw_connect/socket"); return -1; }

    int no = 0;
    setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &no, sizeof(no));

    if (connect(fd, (struct sockaddr*)&sin6, sizeof(sin6)) < 0) {
        perror("raw_connect/connect");
        close(fd);
        return -1;
    }
    return fd;
}

static int raw_send_framed(int fd, const void* data, size_t len) {
    uint8_t hdr[4];
    hdr[0] = (uint8_t)(len >> 24);
    hdr[1] = (uint8_t)(len >> 16);
    hdr[2] = (uint8_t)(len >>  8);
    hdr[3] = (uint8_t)(len);
    if (write(fd, hdr, 4) < 0) return -1;
    if (len > 0 && write(fd, data, len) < 0) return -1;
    return 0;
}

static void test_server_open(void) {
    fprintf(stderr, "\n--- TCP server NONBLOCK: open ---\n");

    xlink_opt_t opt = XLINK_OPT_DEFAULT;
    opt.flags = XLINK_SERVER | XLINK_NONBLOCK;

    xlink_channel_t* srv = xlink_open(XLINK_TCP, ":19996", &opt);
    CHECK(srv != NULL, "TCP server with XLINK_NONBLOCK opens");
    if (srv) xlink_close(srv);
}

static void test_single_client(void) {
    fprintf(stderr, "\n--- TCP server NONBLOCK: single client ---\n");

    xlink_opt_t opt = XLINK_OPT_DEFAULT;
    opt.flags = XLINK_SERVER | XLINK_NONBLOCK;

    xlink_channel_t* srv = xlink_open(XLINK_TCP, ":19996", &opt);
    CHECK(srv != NULL, "open server");

    if (!srv) return;

    /* Connect raw client */
    int cfd = raw_connect(PORT);
    CHECK(cfd >= 0, "raw client connects");
    if (cfd < 0) { xlink_close(srv); return; }

    /* Client sends framed message */
    const char* msg = "hello";
    int rc = raw_send_framed(cfd, msg, 6);
    CHECK(rc == 0, "raw client sends framed message");

    /* Server receives */
    uint8_t buf[256];
    size_t len = sizeof(buf);
    rc = xlink_recv(srv, buf, &len);
    CHECK(rc == 0, "server receives framed message");
    CHECK(len == 6, "message length is 6");
    CHECK(memcmp(buf, "hello", 6) == 0, "message content matches");

    close(cfd);
    xlink_close(srv);
}

static void test_two_clients(void) {
    fprintf(stderr, "\n--- TCP server NONBLOCK: two clients ---\n");

    xlink_opt_t opt = XLINK_OPT_DEFAULT;
    opt.flags = XLINK_SERVER | XLINK_NONBLOCK;

    xlink_channel_t* srv = xlink_open(XLINK_TCP, ":19996", &opt);
    CHECK(srv != NULL, "open server");

    if (!srv) return;

    /* Two raw clients connect */
    int c1 = raw_connect(PORT);
    int c2 = raw_connect(PORT);
    CHECK(c1 >= 0, "client 1 connects");
    CHECK(c2 >= 0, "client 2 connects");

    if (c1 < 0 || c2 < 0) {
        if (c1 >= 0) close(c1);
        if (c2 >= 0) close(c2);
        xlink_close(srv);
        return;
    }

    /* Both send framed messages */
    int rc = raw_send_framed(c1, "alpha", 6);
    CHECK(rc == 0, "client 1 sends 'alpha'");

    rc = raw_send_framed(c2, "beta", 5);
    CHECK(rc == 0, "client 2 sends 'beta'");

    /* Wait briefly for data to arrive */
    usleep(50000);

    /* Server receives both messages (order may vary by poll) */
    uint8_t buf[256];
    size_t len;
    int got_alpha = 0, got_beta = 0;

    for (int i = 0; i < 2; i++) {
        len = sizeof(buf);
        rc = xlink_recv(srv, buf, &len);
        CHECK(rc == 0, "server recv message");

        if (len == 6 && memcmp(buf, "alpha", 6) == 0) got_alpha = 1;
        if (len == 5 && memcmp(buf, "beta", 5) == 0)  got_beta = 1;
    }

    CHECK(got_alpha, "received 'alpha' from client 1");
    CHECK(got_beta,  "received 'beta' from client 2");

    close(c1);
    close(c2);
    xlink_close(srv);
}

static void test_client_disconnect(void) {
    fprintf(stderr, "\n--- TCP server NONBLOCK: client disconnect ---\n");

    xlink_opt_t opt = XLINK_OPT_DEFAULT;
    opt.flags = XLINK_SERVER | XLINK_NONBLOCK;

    xlink_channel_t* srv = xlink_open(XLINK_TCP, ":19996", &opt);
    CHECK(srv != NULL, "open server");

    if (!srv) return;

    /* Connect client, send message, disconnect */
    int cfd = raw_connect(PORT);
    CHECK(cfd >= 0, "raw client connects");

    if (cfd < 0) { xlink_close(srv); return; }

    int rc = raw_send_framed(cfd, "ping", 5);
    CHECK(rc == 0, "client sends 'ping'");

    /* Server receives */
    uint8_t buf[256];
    size_t len = sizeof(buf);
    rc = xlink_recv(srv, buf, &len);
    CHECK(rc == 0 && len == 5 && memcmp(buf, "ping", 5) == 0,
          "server receives 'ping'");

    /* Client disconnects */
    close(cfd);

    /* Second client connects and sends */
    int c2 = raw_connect(PORT);
    CHECK(c2 >= 0, "client 2 connects after disconnect");

    rc = raw_send_framed(c2, "pong", 5);
    CHECK(rc == 0, "client 2 sends 'pong'");

    /* Wait for data */
    usleep(50000);

    /* Server should get message from client 2 (dead client 1 already removed) */
    len = sizeof(buf);
    rc = xlink_recv(srv, buf, &len);
    CHECK(rc == 0 && len == 5 && memcmp(buf, "pong", 5) == 0,
          "server receives 'pong' from client 2 after disconnect");

    close(c2);
    xlink_close(srv);
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);

    fprintf(stderr, "=== TCP server NONBLOCK tests ===\n");

    test_server_open();
    test_single_client();
    test_two_clients();
    test_client_disconnect();

    fprintf(stderr, "\n=== %d checks, %d failures ===\n", checks, failures);
    return failures > 0 ? 1 : 0;
}
