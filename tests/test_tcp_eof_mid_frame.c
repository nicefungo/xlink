/*
 * test_tcp_eof_mid_frame.c — TCP EOF mid-framed-message
 *
 * Verifies that a TCP client receiving a partial framed message
 * (header claims N bytes, connection drops before N bytes arrive)
 * correctly returns an error and does NOT interpret partial
 * payload as a valid message (which would desync the framer).
 *
 * Also tests the server-side multi-client recv: if a client
 * disconnects mid-frame, the server should detect it and remove
 * the client without crashing.
 *
 * Protocol: 4-byte BE length prefix + payload.
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
#include <signal.h>

#define PORT 19898

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

/* Write 4-byte BE framing header only (no payload) */
static void write_frame_hdr(int fd, uint32_t len) {
    uint8_t hdr[4];
    hdr[0] = (uint8_t)(len >> 24);
    hdr[1] = (uint8_t)(len >> 16);
    hdr[2] = (uint8_t)(len >> 8);
    hdr[3] = (uint8_t)(len);
    ssize_t r = write(fd, hdr, 4); (void)r;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);

    int srv = socket(AF_INET6, SOCK_STREAM, 0);
    CHECK(srv >= 0, "server socket");
    if (srv < 0) return 1;

    int no = 0, reuse = 1;
    setsockopt(srv, IPPROTO_IPV6, IPV6_V6ONLY, &no, sizeof(no));
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in6 sin6;
    memset(&sin6, 0, sizeof(sin6));
    sin6.sin6_family = AF_INET6;
    sin6.sin6_port   = htons(PORT);
    sin6.sin6_addr   = in6addr_any;
    if (bind(srv, (struct sockaddr*)&sin6, sizeof(sin6)) < 0) {
        perror("bind"); close(srv); return 1;
    }
    if (listen(srv, 5) < 0) {
        perror("listen"); close(srv); return 1;
    }

    /* ================================================================
     * Test 1: Client receives framing header claiming 2000 bytes,
     * server writes only 500 bytes, then closes connection (EOF).
     * Client should return -1 (not a truncated message).
     * ================================================================ */
    fprintf(stderr, "\n--- TCP client: EOF mid-payload ---\n");

    char addr[32];
    snprintf(addr, sizeof(addr), "127.0.0.1:%d", PORT);

    xlink_channel_t* ch = xlink_open(XLINK_TCP, addr, NULL);
    CHECK(ch != NULL, "xlink_open client");
    if (!ch) { close(srv); return 1; }

    int cfd = accept(srv, NULL, NULL);
    CHECK(cfd >= 0, "server accepts client");
    if (cfd < 0) { close(srv); return 1; }

    /* Server writes header claiming 2000 bytes + only 500 payload bytes */
    write_frame_hdr(cfd, 2000);

    uint8_t partial[500];
    memset(partial, 'Z', sizeof(partial));
    ssize_t r = write(cfd, partial, sizeof(partial)); (void)r;

    /* Close server connection — EOF mid-frame */
    close(cfd);

    /* Client recv → should fail (EOF mid-payload, not a valid message) */
    uint8_t buf[4096];
    size_t len = sizeof(buf);
    int rc = xlink_recv(ch, buf, &len);
    CHECK(rc == -1, "client recv returns -1 after EOF-mid-payload");

    const char* err = xlink_errstr(ch);
    CHECK(err != NULL, "error string non-NULL");
    fprintf(stderr, "  err: %s\n", err);

    xlink_close(ch);
    close(srv);

    /* ================================================================
     * Test 2: Server receives EOF mid-frame from a client.
     * Server is xlink TCP server mode. Client connects, sends partial
     * frame, then disconnects. Server should not crash.
     *
     * Use a different port (19899) to avoid TIME_WAIT conflicts.
     * ================================================================ */
    fprintf(stderr, "\n--- TCP server: client EOF mid-frame ---\n");

    char srv_addr[32];
    snprintf(srv_addr, sizeof(srv_addr), "127.0.0.1:%d", PORT + 1);

    xlink_opt_t opt = XLINK_OPT_DEFAULT;
    opt.flags = XLINK_SERVER;

    xlink_channel_t* srv_ch = xlink_open(XLINK_TCP, srv_addr, &opt);
    CHECK(srv_ch != NULL, "xlink_open server");
    if (!srv_ch) { close(srv); return 1; }
    close(srv);  /* close raw server — xlink server took over */

    /* Short sleep for server to be ready */
    usleep(100000);

    /* Connect a raw client */

    /* Wait for next avail server port — reuse listening port */
    int cl_fd = socket(AF_INET6, SOCK_STREAM, 0);
    CHECK(cl_fd >= 0, "client socket for server test");
    if (cl_fd < 0) { xlink_close(srv_ch); return 1; }

    struct sockaddr_in6 cl_addr;
    memset(&cl_addr, 0, sizeof(cl_addr));
    cl_addr.sin6_family = AF_INET6;
    cl_addr.sin6_port   = htons(PORT + 1);
    cl_addr.sin6_addr   = in6addr_loopback;

    if (connect(cl_fd, (struct sockaddr*)&cl_addr, sizeof(cl_addr)) < 0) {
        perror("connect"); close(cl_fd); xlink_close(srv_ch); return 1;
    }

    /* Send header + partial payload, then disconnect */
    write_frame_hdr(cl_fd, 3000);
    r = write(cl_fd, partial, 100); (void)r;
    close(cl_fd);  /* EOF mid-frame */

    /*
     * Server should NOT crash.  recv_multi sees POLLHUP on client fd,
     * tries to read, gets EOF mid-header/payload, removes the client.
     * Since no valid data was received, xlink_recv will block waiting
     * for more (there's nothing to return). We use a second valid
     * client to verify the server is still alive.
     */
    usleep(100000);

    /* Connect another client that sends a valid message */
    int cl_fd2 = socket(AF_INET6, SOCK_STREAM, 0);
    cl_addr.sin6_port = htons(PORT);
    if (connect(cl_fd2, (struct sockaddr*)&cl_addr, sizeof(cl_addr)) < 0) {
        perror("client2 connect"); close(cl_fd2); xlink_close(srv_ch); return 1;
    }

    /* Write valid framed message via raw socket */
    write_frame_hdr(cl_fd2, 9);
    r = write(cl_fd2, "valid_msg", 9); (void)r;

    /* Server should be able to recv this valid message */
    uint8_t srv_buf[256];
    size_t srv_len = sizeof(srv_buf);
    rc = xlink_recv(srv_ch, srv_buf, &srv_len);
    CHECK(rc == 0, "server recv succeeds after client disconnect mid-frame");
    CHECK(srv_len == 9, "server recv got 'valid_msg'");
    CHECK(memcmp(srv_buf, "valid_msg", 9) == 0, "server recv content matches");

    close(cl_fd2);
    xlink_close(srv_ch);

    /* ================================================================
     * Test 3: Client receives only the 4-byte header, then EOF
     * (no payload at all after header).
     * ================================================================ */
    fprintf(stderr, "\n--- TCP client: EOF mid-header (only partial header) ---\n");

    /* Manually create a raw TCP listening socket for this test */
    srv = socket(AF_INET6, SOCK_STREAM, 0);
    if (srv >= 0) {
        no = 0; reuse = 1;
        setsockopt(srv, IPPROTO_IPV6, IPV6_V6ONLY, &no, sizeof(no));
        setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        memset(&sin6, 0, sizeof(sin6));
        sin6.sin6_family = AF_INET6;
        sin6.sin6_port   = htons(PORT + 1);
        sin6.sin6_addr   = in6addr_any;
        if (bind(srv, (struct sockaddr*)&sin6, sizeof(sin6)) == 0 &&
            listen(srv, 5) == 0) {

            char addr2[32];
            snprintf(addr2, sizeof(addr2), "127.0.0.1:%d", PORT + 1);

            ch = xlink_open(XLINK_TCP, addr2, NULL);
            CHECK(ch != NULL, "xlink_open client for partial-header test");

            if (ch) {
                cfd = accept(srv, NULL, NULL);
                CHECK(cfd >= 0, "server accepts for partial-header test");

                if (cfd >= 0) {
                    /* Write only 2 bytes of the 4-byte header, then close */
                    uint8_t bad_hdr[2] = { 0, 0x7F };
                    r = write(cfd, bad_hdr, 2); (void)r;
                    close(cfd);

                    len = sizeof(buf);
                    rc = xlink_recv(ch, buf, &len);
                    CHECK(rc == -1, "recv returns -1 after partial header (EOF)");
                    fprintf(stderr, "  partial header err: %s\n", xlink_errstr(ch));
                }
                xlink_close(ch);
            }
        }
        close(srv);
    }

    fprintf(stderr, "\n=== %d/%d PASSED ===\n", passed, checks);
    return (checks == passed) ? 0 : 1;
}
