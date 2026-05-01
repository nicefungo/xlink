/*
 * test_tcp_zero.c — Test zero-length framed messages on TCP server/client.
 *
 * Tests the recv_multi path where msglen == 0 (4-byte header with
 * payload_len=0).  Also tests that framing stays in sync after a
 * zero-length message.
 */

#include "xlink.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/wait.h>

static int failures = 0;
static int checks   = 0;
#define CHECK(cond, msg) do {                                           \
    checks++;                                                           \
    if (!(cond)) {                                                      \
        fprintf(stderr, "  FAIL [%d] %s\n", checks, msg);              \
        failures++;                                                     \
    } else {                                                            \
        fprintf(stderr, "  PASS [%d] %s\n", checks, msg);              \
    }                                                                   \
} while(0)

static void test_tcp_zero(void) {
    fprintf(stderr, "\n--- TCP zero-length framed message ---\n");

    int port = 19990;
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(listen_fd >= 0, "create raw listen socket");

    int reuse = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)port);

    int rc = bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr));
    CHECK(rc == 0, "bind raw listen socket");

    rc = listen(listen_fd, 5);
    CHECK(rc == 0, "listen on raw socket");

    /* Fork: child is the raw TCP server, parent is the xlink TCP client */
    pid_t pid = fork();
    CHECK(pid >= 0, "fork for TCP zero test");

    if (pid == 0) {
        /* ── CHILD: raw TCP server ── */
        int cfd = accept(listen_fd, NULL, NULL);
        if (cfd < 0) _exit(1);
        close(listen_fd);

        uint8_t buf[256];

        /* Read message 1: zero-length frame (header only) */
        {
            uint8_t hdr[4];
            if (read(cfd, hdr, 4) != 4) _exit(2);
            uint32_t len = (uint32_t)hdr[0] << 24 | (uint32_t)hdr[1] << 16
                         | (uint32_t)hdr[2] <<  8 | (uint32_t)hdr[3];
            if (len != 0) _exit(3);
            /* Should see "OK" for msg1 acknowledge */
            uint8_t ack[4];
            if (read(cfd, ack, 4) != 4) _exit(4);
            len = (uint32_t)ack[0] << 24 | (uint32_t)ack[1] << 16
                | (uint32_t)ack[2] <<  8 | (uint32_t)ack[3];
            if (len != 2) _exit(5);
            if (read(cfd, buf, 2) != 2) _exit(6);
            if (buf[0] != 'O' || buf[1] != 'K') _exit(7);
        }

        /* Read message 2: normal frame */
        {
            uint8_t hdr[4];
            if (read(cfd, hdr, 4) != 4) _exit(8);
            uint32_t len = (uint32_t)hdr[0] << 24 | (uint32_t)hdr[1] << 16
                         | (uint32_t)hdr[2] <<  8 | (uint32_t)hdr[3];
            if (len != 5) _exit(9);
            if (read(cfd, buf, 5) != 5) _exit(10);
            if (memcmp(buf, "hello", 5) != 0) _exit(11);
        }

        close(cfd);
        _exit(0);
    }

    /* ── PARENT: xlink TCP client ── */

    /* Wait a bit for child to start listening */
    usleep(100000);

    char addr_str[32];
    snprintf(addr_str, sizeof(addr_str), "127.0.0.1:%d", port);

    xlink_opt_t opt = XLINK_OPT_DEFAULT;
    xlink_channel_t* ch = xlink_open(XLINK_TCP, addr_str, &opt);
    CHECK(ch != NULL, "xlink TCP client connects");

    if (ch) {
        int rc2;

        /* Send zero-length message */
        rc2 = xlink_send(ch, "", 0);
        CHECK(rc2 == 0, "send zero-length framed message");

        /* Send "OK" reply */
        rc2 = xlink_send(ch, "OK", 2);
        CHECK(rc2 == 0, "send 'OK' after zero-length message");

        /* Send normal message */
        rc2 = xlink_send(ch, "hello", 5);
        CHECK(rc2 == 0, "send 'hello' after zero-length message");

        xlink_close(ch);
    }

    /* Wait for child */
    int status;
    waitpid(pid, &status, 0);

    if (WIFEXITED(status)) {
        int exit_code = WEXITSTATUS(status);
        CHECK(exit_code == 0, "child server process exits cleanly");
        if (exit_code != 0) {
            fprintf(stderr, "       child exit code = %d\n", exit_code);
        }
    } else {
        CHECK(0, "child server process exits normally");
    }

    close(listen_fd);
}

static void test_tcp_zero_between_normal(void) {
    fprintf(stderr, "\n--- TCP zero-length messages between normal ---\n");

    int port = 19991;
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(listen_fd >= 0, "create listen socket");

    int reuse = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)port);
    bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr));
    listen(listen_fd, 5);

    pid_t pid = fork();
    CHECK(pid >= 0, "fork");

    if (pid == 0) {
        /* CHILD: raw TCP server */
        int cfd = accept(listen_fd, NULL, NULL);
        if (cfd < 0) _exit(1);
        close(listen_fd);

        uint8_t hdr[4], buf[256];

        /* 1) Receive "first" (5 bytes) */
        if (read(cfd, hdr, 4) != 4) _exit(2);
        uint32_t len = (uint32_t)hdr[0]<<24|(uint32_t)hdr[1]<<16
                     | (uint32_t)hdr[2]<<8|(uint32_t)hdr[3];
        if (len != 5) _exit(3);
        if (read(cfd, buf, 5) != 5) _exit(4);
        if (memcmp(buf, "first", 5) != 0) _exit(5);

        /* 2) Receive zero-length message */
        if (read(cfd, hdr, 4) != 4) _exit(6);
        len = (uint32_t)hdr[0]<<24|(uint32_t)hdr[1]<<16
            | (uint32_t)hdr[2]<<8|(uint32_t)hdr[3];
        if (len != 0) _exit(7);

        /* 3) Receive "second" (6 bytes) */
        if (read(cfd, hdr, 4) != 4) _exit(8);
        len = (uint32_t)hdr[0]<<24|(uint32_t)hdr[1]<<16
            | (uint32_t)hdr[2]<<8|(uint32_t)hdr[3];
        if (len != 6) _exit(9);
        if (read(cfd, buf, 6) != 6) _exit(10);
        if (memcmp(buf, "second", 6) != 0) _exit(11);

        /* 4) Receive zero-length message */
        if (read(cfd, hdr, 4) != 4) _exit(12);
        len = (uint32_t)hdr[0]<<24|(uint32_t)hdr[1]<<16
            | (uint32_t)hdr[2]<<8|(uint32_t)hdr[3];
        if (len != 0) _exit(13);

        close(cfd);
        _exit(0);
    }

    /* PARENT */
    usleep(100000);

    char addr_str[32];
    snprintf(addr_str, sizeof(addr_str), "127.0.0.1:%d", port);

    xlink_opt_t opt = XLINK_OPT_DEFAULT;
    xlink_channel_t* ch = xlink_open(XLINK_TCP, addr_str, &opt);
    CHECK(ch != NULL, "connect for zero-between test");

    if (ch) {
        /* "first" → zero → "second" → zero */
        CHECK(xlink_send(ch, "first", 5) == 0,  "send 'first'");
        CHECK(xlink_send(ch, "", 0) == 0,        "send zero-length");
        CHECK(xlink_send(ch, "second", 6) == 0,  "send 'second'");
        CHECK(xlink_send(ch, "", 0) == 0,        "send zero-length #2");

        xlink_close(ch);
    }

    int status;
    waitpid(pid, &status, 0);

    if (WIFEXITED(status)) {
        int exit_code = WEXITSTATUS(status);
        CHECK(exit_code == 0, "zero-between child exits cleanly");
        if (exit_code != 0)
            fprintf(stderr, "       child exit code = %d\n", exit_code);
    } else {
        CHECK(0, "zero-between child exits normally");
    }

    close(listen_fd);
}

int main(void) {
    fprintf(stderr, "=== Test TCP zero-length framed messages ===\n");
    test_tcp_zero();
    test_tcp_zero_between_normal();
    fprintf(stderr, "\n=== RESULTS: %d checks, %d failures ===\n",
            checks, failures);
    return failures > 0 ? 1 : 0;
}
