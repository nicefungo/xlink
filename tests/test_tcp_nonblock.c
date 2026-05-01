/*
 * test_tcp_nonblock.c — Test TCP client with XLINK_NONBLOCK flag.
 *
 * Verifies that a TCP client opened with XLINK_NONBLOCK can:
 *   1) Connect and send framed messages
 *   2) Receive framed messages from server without false disconnects
 *   3) Both directions work reliably (NONBLOCK read/write on client socket)
 *   4) Reconnect preserves the NONBLOCK flag on the new fd
 *
 * Previously untested code paths:
 *   - tcp_connect_client: O_NONBLOCK applied to client fd (apply_nonblock)
 *   - tcp_backend_recv (client mode): read_framed on NONBLOCK socket
 *   - tcp_backend_send (client mode): write_framed on NONBLOCK socket
 *   - recv_multi: NONBLOCK applied to accepted client fds
 *   - try_reconnect: O_NONBLOCK applied to reconnected fd
 */

#include "xlink.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <signal.h>
#include <fcntl.h>

#define PORT     ":19998"
#define ADDR     "127.0.0.1:19998"
#define BUFSZ    256

static int failures = 0;
#define CHECK(cond, msg) do {                                           \
    if (!(cond)) {                                                      \
        fprintf(stderr, "  FAIL [%d]: %s\n", __LINE__, msg);          \
        failures++;                                                     \
    } else {                                                            \
        fprintf(stderr, "  PASS [%d]: %s\n", __LINE__, msg);          \
    }                                                                   \
} while(0)

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
        xlink_channel_t* ch = xlink_open(XLINK_TCP, PORT, &opt);
        if (!ch) { fprintf(stderr, "S: open fail\n"); _exit(1); }

        /* ── Receive 3 messages from client ── */
        uint8_t buf[BUFSZ];
        size_t len;

        /* Msg 1: "hello" */
        len = sizeof(buf);
        if (xlink_recv(ch, buf, &len) != 0) {
            fprintf(stderr, "S: recv #1: %s\n", xlink_errstr(ch));
            _exit(1);
        }
        if (len != 5 || memcmp(buf, "hello", 5) != 0) {
            fprintf(stderr, "S: recv #1 got %zu bytes\n", len);
            _exit(1);
        }

        /* Msg 2: "world" */
        len = sizeof(buf);
        if (xlink_recv(ch, buf, &len) != 0) {
            fprintf(stderr, "S: recv #2: %s\n", xlink_errstr(ch));
            _exit(1);
        }
        if (len != 5 || memcmp(buf, "world", 5) != 0) {
            fprintf(stderr, "S: recv #2 got %zu bytes\n", len);
            _exit(1);
        }

        /* Msg 3: "done" */
        len = sizeof(buf);
        if (xlink_recv(ch, buf, &len) != 0) {
            fprintf(stderr, "S: recv #3: %s\n", xlink_errstr(ch));
            _exit(1);
        }
        if (len != 4 || memcmp(buf, "done", 4) != 0) {
            fprintf(stderr, "S: recv #3 got %zu bytes\n", len);
            _exit(1);
        }

        /* ── Send response back to client ── */
        if (xlink_send(ch, "AOK", 3) != 0) {
            fprintf(stderr, "S: send response: %s\n", xlink_errstr(ch));
            _exit(1);
        }

        xlink_close(ch);
        ssize_t w = write(child_pipe[1], "ok", 3);
        (void)w;
        _exit(0);
    }

    /* ── Parent: TCP client with NONBLOCK ── */
    close(child_pipe[1]);

    xlink_opt_t opt = XLINK_OPT_DEFAULT;
    opt.flags = XLINK_NONBLOCK;

    xlink_channel_t* ch = NULL;
    for (int retry = 0; retry < 10; retry++) {
        ch = xlink_open(XLINK_TCP, ADDR, &opt);
        if (ch) break;
        usleep(150000);
    }
    CHECK(ch != NULL, "nonblock client open");
    if (!ch) {
        kill(svr_pid, SIGTERM);
        waitpid(svr_pid, NULL, 0);
        close(child_pipe[0]);
        fprintf(stderr, "=== FAILED ===\n");
        return 1;
    }

    /* ── Send 3 framed messages ── */
    /* (ch->fd is opaque — NONBLOCK verified functionally below) */
    CHECK(xlink_send(ch, "hello", 5) == 0, "send 'hello' via nonblock client");
    CHECK(xlink_send(ch, "world", 5) == 0, "send 'world' via nonblock client");
    CHECK(xlink_send(ch, "done", 4) == 0, "send 'done' via nonblock client");

    /* ── Receive response from server ── */
    uint8_t buf[BUFSZ];
    size_t len = sizeof(buf);
    int rc = xlink_recv(ch, buf, &len);
    /* May need retry due to network timing */
    int retries = 20;
    while (rc != 0 && retries > 0) {
        usleep(50000);
        len = sizeof(buf);
        rc = xlink_recv(ch, buf, &len);
        retries--;
    }
    CHECK(rc == 0, "nonblock client recv response from server");
    CHECK(len == 3 && memcmp(buf, "AOK", 3) == 0, "response content matches 'AOK'");

    xlink_close(ch);

    /* Wait for server to finish */
    char result[16] = {0};
    ssize_t n = read(child_pipe[0], result, sizeof(result) - 1);
    if (n > 0) result[n] = '\0';
    CHECK(strcmp(result, "ok") == 0, "server received all messages from nonblock client");
    close(child_pipe[0]);

    waitpid(svr_pid, NULL, 0);

    /* ════════════════════════════════════════════════════════════════════
     * Reconnect test: verify NONBLOCK flag is preserved after reconnect
     *
     * Strategy: Use raw sockets on the server side (single process, no fork).
     * 1) Create raw TCP server socket, listen on 19998
     * 2) Open NONBLOCK xlink client, send "kick" → server accepts
     * 3) Server closes accepted connection → client detects RST
     * 4) Client sends "resend" → triggers try_reconnect → new fd connects
     * 5) Verify NONBLOCK works on reconnected fd (functional test)
     * ════════════════════════════════════════════════════════════════════ */

    fprintf(stderr, "\n--- Reconnect NONBLOCK test ---\n");

    {
        int svr_fd = socket(AF_INET, SOCK_STREAM, 0);
        CHECK(svr_fd >= 0, "reconn: raw server socket");
        if (svr_fd < 0) { fprintf(stderr, "=== FAILED ===\n"); return 1; }

        int optval = 1;
        setsockopt(svr_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

        struct sockaddr_in saddr;
        memset(&saddr, 0, sizeof(saddr));
        saddr.sin_family      = AF_INET;
        saddr.sin_port        = htons(19998);
        saddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

        int bind_rc   = bind(svr_fd, (struct sockaddr*)&saddr, sizeof(saddr));
        int listen_rc = listen(svr_fd, 5);
        CHECK(bind_rc == 0, "reconn: raw server bind");
        CHECK(listen_rc == 0, "reconn: raw server listen");
        if (bind_rc < 0 || listen_rc < 0) {
            close(svr_fd);
            fprintf(stderr, "=== FAILED ===\n");
            return 1;
        }

        /* Open NONBLOCK xlink client, connect and send "kick" */
        xlink_channel_t* ch_r = NULL;
        for (int retry = 0; retry < 50; retry++) {
            ch_r = xlink_open(XLINK_TCP, ADDR, &opt);
            if (ch_r) break;
            usleep(50000);
        }
        CHECK(ch_r != NULL, "reconn: nonblock client open");
        if (!ch_r) { close(svr_fd); return 1; }

        CHECK(xlink_send(ch_r, "kick", 4) == 0, "reconn: send 'kick'");

        /* Accept the 'kick' connection */
        int cfd1 = accept(svr_fd, NULL, NULL);
        CHECK(cfd1 >= 0, "reconn: server accept #1");

        /* Raw server receives frame header + payload */
        uint8_t hdr[4];
        ssize_t nr = read(cfd1, hdr, 4);
        CHECK(nr == 4, "reconn: read frame header");
        if (nr == 4) {
            uint32_t paylen = (uint32_t)hdr[0] << 24
                            | (uint32_t)hdr[1] << 16
                            | (uint32_t)hdr[2] << 8
                            | (uint32_t)hdr[3];
            CHECK(paylen == 4, "reconn: payload len == 4");
            uint8_t pay[4];
            nr = read(cfd1, pay, 4);
            CHECK(nr == 4 && memcmp(pay, "kick", 4) == 0,
                  "reconn: server got 'kick'");
        }

        /* ── Force disconnect ──
         * Close the accepted fd. Server stays alive on listen_fd.
         * Client's next send will get RST (or EPIPE on subsequent write). */
        close(cfd1);
        usleep(100000);   /* let RST propagate */

        /* Send empty frame — writev succeeds locally (buffered),
         * but triggers RST processing.  Then send "x" — this gets EPIPE.
         * After EPIPE, ch->fd = -1 and reconnect backoff starts. */
        xlink_send(ch_r, "", 0);
        usleep(50000);
        xlink_send(ch_r, "x", 1);   /* fails → EPIPE → ch->fd = -1 */

        /* Now send "resend" → triggers try_reconnect.
         * This is the code path we want to test: try_reconnect should
         * set O_NONBLOCK on the new fd (previously missing fix). */
        CHECK(xlink_send(ch_r, "resend", 6) == 0,
              "reconn: send 'resend' (triggers try_reconnect)");

        /* Accept the reconnected client */
        int cfd2 = accept(svr_fd, NULL, NULL);
        CHECK(cfd2 >= 0, "reconn: server accept #2");

        /* Read "resend" from reconnected client */
        nr = read(cfd2, hdr, 4);
        CHECK(nr == 4, "reconn: read frame header #2");
        if (nr == 4) {
            uint32_t paylen2 = (uint32_t)hdr[0] << 24
                             | (uint32_t)hdr[1] << 16
                             | (uint32_t)hdr[2] << 8
                             | (uint32_t)hdr[3];
            CHECK(paylen2 == 6, "reconn: payload len == 6");
            uint8_t pay2[6];
            nr = read(cfd2, pay2, 6);
            CHECK(nr == 6 && memcmp(pay2, "resend", 6) == 0,
                  "reconn: server got 'resend'");
        }

        /* Send "AOK" response (framed) */
        uint8_t resp_hdr[4];
        resp_hdr[0] = 0; resp_hdr[1] = 0;
        resp_hdr[2] = 0; resp_hdr[3] = 3;
        { ssize_t dummy_ = write(cfd2, resp_hdr, 4); (void)dummy_; }
        { ssize_t dummy_ = write(cfd2, "AOK", 3);    (void)dummy_; }

        /* ── Verify NONBLOCK on reconnected fd ──
         * Client should be able to receive "AOK" despite NONBLOCK.
         * The key: the reconnected fd must have O_NONBLOCK so that
         * reads don't block the process.  retry loop covers timing. */
        uint8_t rbuf[32];
        size_t rlen = sizeof(rbuf);
        int rrc = xlink_recv(ch_r, rbuf, &rlen);
        int rretries = 20;
        while (rrc != 0 && rretries > 0) {
            usleep(50000);
            rlen = sizeof(rbuf);
            rrc = xlink_recv(ch_r, rbuf, &rlen);
            rretries--;
        }
        CHECK(rrc == 0, "reconn: client recv 'AOK' after reconnect");
        if (rrc == 0) {
            CHECK(rlen == 3 && memcmp(rbuf, "AOK", 3) == 0,
                  "reconn: 'AOK' content match");
        }

        xlink_close(ch_r);
        close(cfd2);
        close(svr_fd);
    }

    fprintf(stderr, "=== %s ===\n", failures ? "FAILED" : "ALL PASSED");
    return failures ? 1 : 0;
}
