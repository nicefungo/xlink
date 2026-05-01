/*
 * UDP edge case tests
 *
 * Tests the XLINK_CREATE flag forcing receiver mode with a host:port
 * address, and verifies the receiver path works.
 */

#include "xlink.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/wait.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static int failures = 0;
#define CHECK(cond, msg) do {                                   \
    if (!(cond)) {                                              \
        fprintf(stderr, "  FAIL: %s\n", msg);                   \
        failures++;                                             \
    } else {                                                    \
        printf("  PASS: %s\n", msg);                            \
    }                                                           \
} while(0)

static int raw_udp_send(int port, const void* data, size_t len) {
    int fd = socket(AF_INET6, SOCK_DGRAM, 0);
    if (fd < 0) return -1;

    int no = 0;
    setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &no, sizeof(no));

    struct sockaddr_in6 sin6;
    memset(&sin6, 0, sizeof(sin6));
    sin6.sin6_family = AF_INET6;
    sin6.sin6_port   = htons((uint16_t)port);
    /* 127.0.0.1 as IPv4-mapped IPv6 */
    sin6.sin6_addr.s6_addr[10] = 0xff;
    sin6.sin6_addr.s6_addr[11] = 0xff;
    sin6.sin6_addr.s6_addr[12] = 127;
    sin6.sin6_addr.s6_addr[15] = 1;

    ssize_t n;
    do { n = sendto(fd, data, len, 0, (struct sockaddr*)&sin6, sizeof(sin6)); }
    while (n < 0 && errno == EINTR);
    close(fd);
    return (n >= 0) ? 0 : -1;
}

static int test_udp_nonblock_eagain(void) {
    printf("\n--- UDP NONBLOCK recv EAGAIN path ---\n");

    int port = 19989;

    /* Open UDP receiver with NONBLOCK */
    char addr[32];
    snprintf(addr, sizeof(addr), ":%d", port);

    xlink_opt_t opt = XLINK_OPT_DEFAULT;
    opt.flags = XLINK_NONBLOCK;

    xlink_channel_t* ch = xlink_open(XLINK_UDP, addr, &opt);
    CHECK(ch != NULL, "UDP NONBLOCK receiver open");

    if (!ch) return 1;

    /* NONBLOCK recv on empty socket should return -1 (EAGAIN) */
    uint8_t buf[256];
    size_t len = sizeof(buf);
    int rc = xlink_recv(ch, buf, &len);
    CHECK(rc == -1, "UDP NONBLOCK recv on empty socket returns -1 (EAGAIN)");

    /* Send a datagram via raw socket */
    const char* msg = "NONBLOCK_EAGAIN test";
    rc = raw_udp_send(port, msg, strlen(msg) + 1);
    CHECK(rc == 0, "raw UDP send to NONBLOCK receiver");

    if (rc == 0) {
        /* Now recv should succeed */
        char buf2[256] = {0};
        size_t len2 = sizeof(buf2);
        rc = xlink_recv(ch, buf2, &len2);
        CHECK(rc == 0, "UDP NONBLOCK recv succeeds after data arrives");
        if (rc == 0) {
            CHECK(len2 == strlen(msg) + 1 &&
                  memcmp(buf2, msg, len2) == 0,
                  "UDP NONBLOCK recv content matches");
        }
    }

    xlink_close(ch);
    return 0;
}

static int test_create_receiver(void) {
    printf("\n--- UDP XLINK_CREATE forces receiver mode ---\n");

    int port = 19995;

    /* Open UDP channel with XLINK_CREATE + host:port address.
     * This should force receiver mode (bind to port). */
    char addr[32];
    snprintf(addr, sizeof(addr), "127.0.0.1:%d", port);

    xlink_opt_t opt = XLINK_OPT_DEFAULT;
    opt.flags = XLINK_CREATE;

    xlink_channel_t* ch = xlink_open(XLINK_UDP, addr, &opt);
    CHECK(ch != NULL, "UDP open with XLINK_CREATE + host:port");

    if (!ch) return 1;

    /* Send a datagram via raw socket */
    const char* msg = "UDP_CREATE receiver test";
    int rc = raw_udp_send(port, msg, strlen(msg) + 1);
    CHECK(rc == 0, "raw UDP send succeeds");

    if (rc == 0) {
        /* Receive via xlink */
        uint8_t buf[256];
        size_t len = sizeof(buf);
        rc = xlink_recv(ch, buf, &len);
        CHECK(rc == 0, "xlink_recv on receiver channel");

        if (rc == 0) {
            CHECK(len == strlen(msg) + 1 &&
                  memcmp(buf, msg, len) == 0,
                  "received content matches sent data");
        }
    }

    xlink_close(ch);
    return 0;
}

int main(void) {
    printf("=== xlink UDP edge case tests ===\n");
    test_create_receiver();
    test_udp_nonblock_eagain();
    printf("\n=== %s ===\n", failures ? "FAILED" : "ALL PASSED");
    return failures ? 1 : 0;
}
