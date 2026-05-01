/*
 * TCP overflow discard test: verify that messages exceeding the recv buffer
 * are silently discarded without breaking framing sync.
 *
 * Server uses a tiny recv buffer (64 bytes). Raw TCP client sends:
 *   1) A large framed message (512 bytes) — > 64 byte buffer
 *   2) A small valid framed message ("small_ok")
 *
 * Server must discard #1 silently and receive #2 intact.
 */

#include "xlink.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <arpa/inet.h>

#define PORT    19993
#define PORT_STR ":19993"
#define ADDR_STR "127.0.0.1:19993"
#define TINY_BUF 64        /* intentionally tiny recv buffer */
#define LARGE_MSG_SZ 512   /* oversized message (single-chunk discard) */
#define HUGE_MSG_SZ 8192  /* very oversized message (multi-chunk discard > 4096) */

static int failures = 0;
#define CHECK(cond, msg) do {                                   \
    if (!(cond)) {                                              \
        fprintf(stderr, "  FAIL: %s\n", msg);                  \
        failures++;                                             \
    } else {                                                    \
        fprintf(stderr, "  PASS: %s\n", msg);                  \
    }                                                           \
} while(0)

/* Write 4-byte big-endian framing header + payload to a raw socket */
static void raw_send_framed(int fd, const void* data, size_t len) {
    uint8_t hdr[4];
    hdr[0] = (uint8_t)(len >> 24);
    hdr[1] = (uint8_t)(len >> 16);
    hdr[2] = (uint8_t)(len >> 8);
    hdr[3] = (uint8_t)(len);
    ssize_t r;
    r = write(fd, hdr, 4); (void)r;
    r = write(fd, data, len); (void)r;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);

    int child_pipe[2];
    if (pipe(child_pipe) != 0) { perror("pipe"); return 1; }

    pid_t svr_pid = fork();
    if (svr_pid == 0) {
        /* ── Server process ── */
        close(child_pipe[0]);

        xlink_opt_t opt = XLINK_OPT_DEFAULT;
        opt.flags = XLINK_SERVER;
        xlink_channel_t* ch = xlink_open(XLINK_TCP, PORT_STR, &opt);
        if (!ch) { fprintf(stderr, "S: open fail\n"); _exit(1); }

        /*
         * First recv: large message (512 bytes) should be silently discarded
         * by the multi-server backend since our buffer (64 bytes) is too small.
         * recv_multi discards it and tries the next.
         */
        uint8_t buf[TINY_BUF];
        size_t len = sizeof(buf);
        int rc = xlink_recv(ch, buf, &len);

        if (rc != 0) {
            fprintf(stderr, "S: first recv: %s\n", xlink_errstr(ch));
            _exit(1);
        }

        /* Verify we got the second (small) message after discard */
        const char* expected = "small_ok";
        size_t exlen = strlen(expected) + 1;
        if (len != exlen || memcmp(buf, expected, exlen) != 0) {
            fprintf(stderr, "S: content mismatch: %zu bytes vs expected %zu\n", len, exlen);
            _exit(1);
        }

        xlink_close(ch);

        /* Signal parent: server done successfully */
        ssize_t w = write(child_pipe[1], "ok", 3);
        (void)w;
        _exit(0);
    }

    /* ── Parent: raw TCP client ── */
    close(child_pipe[1]);

    /* Wait for server to be ready */
    usleep(300000);

    /* Create raw TCP connection */
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(fd >= 0, "create raw socket");

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port   = htons(PORT);
    sa.sin_addr.s_addr = inet_addr("127.0.0.1");

    int rc = connect(fd, (struct sockaddr*)&sa, sizeof(sa));
    CHECK(rc == 0, "raw connect to server");

    if (rc == 0) {
        /* Send huge message (> 4096 bytes) — multi-chunk discard */
        uint8_t huge[HUGE_MSG_SZ];
        memset(huge, 'H', sizeof(huge));
        raw_send_framed(fd, huge, sizeof(huge));
        CHECK(1, "sent huge framed msg (multi-chunk discard)");

        usleep(50000);

        /* Send large message (> TINY_BUF) — single-chunk discard */
        uint8_t large[LARGE_MSG_SZ];
        memset(large, 'X', sizeof(large));
        raw_send_framed(fd, large, sizeof(large));
        CHECK(1, "sent large framed msg (single-chunk discard)");

        /* Small delay to let server process the discard */
        usleep(50000);

        /* Send third, small message — should survive both discards */
        const char* small = "small_ok";
        raw_send_framed(fd, small, strlen(small) + 1);
        CHECK(1, "sent small framed msg");

        /* Wait for server to finish */
        close(fd);
    }

    /* Check server result */
    char result[16] = {0};
    ssize_t n = read(child_pipe[0], result, sizeof(result) - 1);
    if (n > 0) result[n] = '\0';

    CHECK(strcmp(result, "ok") == 0, "server received small msg after overflow discard");

    close(child_pipe[0]);
    waitpid(svr_pid, NULL, 0);

    fprintf(stderr, "=== %s ===\n", failures ? "FAILED" : "ALL PASSED");
    return failures ? 1 : 0;
}
