/*
 * TCP backend test — server + client in same process via fork().
 *
 * Server listens, client connects, sends message, server receives & verifies.
 */

#include "xlink.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>

static const char* ADDR = ":19991";   /* arbitrary high port */

static int test_roundtrip(void) {
    const char* msg = "Hello via TCP!";
    size_t      msglen = strlen(msg) + 1;

    /* ── Server: listen + accept (blocks until client connects) ── */
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return 1;
    }

    if (pid == 0) {
        /* CHILD: server */
        xlink_opt_t opt = XLINK_OPT_DEFAULT;
        opt.flags = XLINK_SERVER;

        xlink_channel_t* ch = xlink_open(XLINK_TCP, ADDR, &opt);
        if (!ch) {
            fprintf(stderr, "FAIL: server open\n");
            _exit(1);
        }

        uint8_t buf[4096];
        size_t  len = sizeof(buf);
        if (xlink_recv(ch, buf, &len) != 0) {
            fprintf(stderr, "FAIL: server recv: %s\n", xlink_errstr(ch));
            xlink_close(ch);
            _exit(1);
        }

        if (len != msglen || memcmp(buf, msg, msglen) != 0) {
            fprintf(stderr, "FAIL: server mismatch (%zu vs %zu)\n",
                    len, msglen);
            xlink_close(ch);
            _exit(1);
        }

        xlink_close(ch);
        _exit(0);
    }

    /* PARENT: client — retry connect a few times */
    xlink_opt_t opt = XLINK_OPT_DEFAULT;
    char client_addr[64];
    snprintf(client_addr, sizeof(client_addr), "127.0.0.1%s", ADDR);

    xlink_channel_t* ch = NULL;
    for (int attempt = 0; attempt < 10; attempt++) {
        ch = xlink_open(XLINK_TCP, client_addr, &opt);
        if (ch) break;
        usleep(100000);   /* 100ms between attempts */
    }

    if (!ch) {
        fprintf(stderr, "FAIL: client open\n");
        kill(pid, SIGTERM);
        waitpid(pid, NULL, 0);
        return 1;
    }

    if (xlink_send(ch, msg, msglen) != 0) {
        fprintf(stderr, "FAIL: client send: %s\n", xlink_errstr(ch));
        xlink_close(ch);
        kill(pid, SIGTERM);
        waitpid(pid, NULL, 0);
        return 1;
    }

    xlink_close(ch);

    /* Wait for server to finish */
    int status;
    waitpid(pid, &status, 0);

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "FAIL: server exited with status %d\n",
                WEXITSTATUS(status));
        return 1;
    }

    printf("  TCP round-trip: %zu bytes OK\n", msglen);
    return 0;
}

int main(void) {
    /* Ignore SIGPIPE so we get EPIPE instead */
    signal(SIGPIPE, SIG_IGN);

    int failures = 0;
    printf("=== xlink TCP test ===\n");
    failures += test_roundtrip();
    printf("=== %s ===\n", failures ? "FAILED" : "ALL PASSED");
    return failures ? 1 : 0;
}
