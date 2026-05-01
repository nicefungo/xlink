/*
 * test_tcp_overflow_client.c — TCP client oversized message discard
 *
 * Verifies that a TCP client receiving a framed message too large for
 * the user buffer discards the payload cleanly (no framing desync)
 * and does NOT trigger a disconnect/reconnect cycle.
 *
 * Server sends: LARGE msg (512 bytes) → "small" msg
 * Client reads with tiny buffer (64 bytes): should discard LARGE,
 * then receive "small" intact on the same connection.
 */

#include "xlink.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>

#define PORT 19897

/* Raw framed write: header (4B BE) + payload */
static void raw_write_framed(int fd, const void* data, size_t len) {
    uint8_t hdr[4];
    hdr[0] = (uint8_t)(len >> 24);
    hdr[1] = (uint8_t)(len >> 16);
    hdr[2] = (uint8_t)(len >> 8);
    hdr[3] = (uint8_t)(len);
    ssize_t dummy;
    dummy = write(fd, hdr, 4); (void)dummy;
    dummy = write(fd, data, len); (void)dummy;
}

/* Build a LARGE payload (exceeds client's 64-byte buffer) */
#define LARGE_SZ 512
static char large_payload[LARGE_SZ];

static int checks = 0, passed = 0;

#define CHECK(cond, msg) do { \
    checks++; \
    if (!(cond)) { \
        fprintf(stderr, "  FAIL [%d] %s\n", checks, msg); \
    } else { \
        fprintf(stderr, "  PASS [%d] %s\n", checks, msg); \
        passed++; \
    } \
} while(0)

int main(void) {
    /* Fill large payload with known pattern */
    for (int i = 0; i < LARGE_SZ; i++)
        large_payload[i] = (char)('A' + (i % 26));

    /* Fork a raw TCP server (same process, no fork) */
    int srv = socket(AF_INET6, SOCK_STREAM, 0);
    if (srv < 0) { perror("server socket"); return 1; }
    int no = 0, reuse = 1;
    setsockopt(srv, IPPROTO_IPV6, IPV6_V6ONLY, &no, sizeof(no));
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in6 sin6;
    memset(&sin6, 0, sizeof(sin6));
    sin6.sin6_family = AF_INET6;
    sin6.sin6_port   = htons(PORT);
    sin6.sin6_addr   = in6addr_any;
    if (bind(srv, (struct sockaddr*)&sin6, sizeof(sin6)) < 0) { perror("bind"); close(srv); return 1; }
    if (listen(srv, 5) < 0) { perror("listen"); close(srv); return 1; }

    /*
     * Test 1: oversized message → discard → next message works
     */
    fprintf(stderr, "\n--- TCP client oversized discard ---\n");

    /* Open xlink client with 64-byte small internal buffer test.
     * We'll use xlink_recv with a real 64-byte buffer to simulate. */
    xlink_opt_t opt = XLINK_OPT_DEFAULT;
    char addr[32];
    snprintf(addr, sizeof(addr), "127.0.0.1:%d", PORT);

    xlink_channel_t* ch = xlink_open(XLINK_TCP, addr, &opt);
    CHECK(ch != NULL, "xlink_open client works");
    if (!ch) { close(srv); return 1; }

    /* Accept client on server side */
    int cfd = accept(srv, NULL, NULL);
    CHECK(cfd >= 0, "server accepts client");
    if (cfd < 0) { close(srv); return 1; }

    /* Server sends LARGE (512 bytes) then "small_ok" */
    raw_write_framed(cfd, large_payload, LARGE_SZ);
    raw_write_framed(cfd, "small_ok", 8);

    /* Client reads with a 64-byte buffer — oversized! */
    {
        uint8_t buf[64];
        size_t len = sizeof(buf);
        int rc = xlink_recv(ch, buf, &len);
        CHECK(rc == -1, "oversized message returns -1");
        /* errno should be ENOSPC after discard */
        CHECK(errno == ENOSPC, "errno is ENOSPC after oversized discard");
    }

    /* Client reads again — should get "small_ok" intact (same connection) */
    {
        uint8_t buf[64];
        size_t len = sizeof(buf);
        int rc = xlink_recv(ch, buf, &len);
        CHECK(rc == 0, "second recv succeeds");
        CHECK(len == 8, "second message length is 8");
        CHECK(memcmp(buf, "small_ok", 8) == 0, "second message content is 'small_ok'");
    }

    xlink_close(ch);
    close(cfd);

    /*
     * Test 2: multiple oversized messages in sequence
     */
    fprintf(stderr, "\n--- Multiple oversized messages ---\n");

    ch = xlink_open(XLINK_TCP, addr, &opt);
    CHECK(ch != NULL, "reconnect for multi-oversized test");
    if (!ch) { close(srv); return 1; }

    cfd = accept(srv, NULL, NULL);
    CHECK(cfd >= 0, "server accepts second client");

    /* Send: LARGE → LARGE → "done" */
    raw_write_framed(cfd, large_payload, LARGE_SZ);
    raw_write_framed(cfd, large_payload, LARGE_SZ);
    raw_write_framed(cfd, "done", 4);

    /* Discard first oversized */
    {
        uint8_t buf[64];
        size_t len = sizeof(buf);
        int rc = xlink_recv(ch, buf, &len);
        CHECK(rc == -1 && errno == ENOSPC, "first oversized discard (ENOSPC)");
    }

    /* Discard second oversized */
    {
        uint8_t buf[64];
        size_t len = sizeof(buf);
        int rc = xlink_recv(ch, buf, &len);
        CHECK(rc == -1 && errno == ENOSPC, "second oversized discard (ENOSPC)");
    }

    /* Read "done" */
    {
        uint8_t buf[64];
        size_t len = sizeof(buf);
        int rc = xlink_recv(ch, buf, &len);
        CHECK(rc == 0, "recv 'done' after double discard");
        CHECK(len == 4, "'done' message length is 4");
        CHECK(memcmp(buf, "done", 4) == 0, "'done' content matches");
    }

    xlink_close(ch);
    close(cfd);

    /*
     * Test 3: oversized messages don't trigger disconnect
     * Verify ch->fd stays open (no reconnect cycle)
     */
    fprintf(stderr, "\n--- Verify no disconnect on oversized ---\n");

    ch = xlink_open(XLINK_TCP, addr, &opt);
    CHECK(ch != NULL, "reconnect for no-disconnect test");

    cfd = accept(srv, NULL, NULL);
    CHECK(cfd >= 0, "server accepts third client");

    raw_write_framed(cfd, large_payload, LARGE_SZ);

    {
        uint8_t buf[64];
        size_t len = sizeof(buf);
        int rc = xlink_recv(ch, buf, &len);
        CHECK(rc == -1, "oversized returns -1 (no disconnect)");
    }

    /* Send a valid message that proves the fd is still open */
    raw_write_framed(cfd, "confirm", 7);

    {
        uint8_t buf[64];
        size_t len = sizeof(buf);
        int rc = xlink_recv(ch, buf, &len);
        CHECK(rc == 0, "still connected — recv 'confirm'");
        CHECK(len == 7, "confirm msg len is 7");
        CHECK(memcmp(buf, "confirm", 7) == 0, "confirm content matches");
    }

    xlink_close(ch);
    close(cfd);
    close(srv);

    fprintf(stderr, "\n=== %d/%d PASSED ===\n", passed, checks);
    return (checks == passed) ? 0 : 1;
}
