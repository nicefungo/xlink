/*
 * UDP backend test — server + client in same process via fork().
 *
 * Uses localhost:PORT for loopback transport.
 */

#include "xlink.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>

/* Sender and receiver use different ports to avoid self-recv */
static const char* RECV_ADDR = ":19993";   /* receiver binds here */
static const char* SEND_ADDR = "127.0.0.1:19993";   /* sender targets recv */

static int test_roundtrip(void) {
    const char* msg = "Hello via UDP!";
    size_t      msglen = strlen(msg) + 1;

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return 1;
    }

    if (pid == 0) {
        /* CHILD: receiver (bind to RECV_ADDR) */
        xlink_opt_t opt = XLINK_OPT_DEFAULT;

        xlink_channel_t* ch = xlink_open(XLINK_UDP, RECV_ADDR, &opt);
        if (!ch) {
            fprintf(stderr, "FAIL: receiver open\n");
            _exit(1);
        }

        uint8_t buf[4096];
        size_t  len = sizeof(buf);
        if (xlink_recv(ch, buf, &len) != 0) {
            fprintf(stderr, "FAIL: receiver recv: %s\n", xlink_errstr(ch));
            xlink_close(ch);
            _exit(1);
        }

        if (len != msglen || memcmp(buf, msg, msglen) != 0) {
            fprintf(stderr, "FAIL: data mismatch (%zu vs %zu)\n",
                    len, msglen);
            xlink_close(ch);
            _exit(1);
        }

        xlink_close(ch);
        _exit(0);
    }

    /* PARENT: sender — give receiver time to bind */
    usleep(200000);

    xlink_opt_t opt = XLINK_OPT_DEFAULT;

    xlink_channel_t* ch = xlink_open(XLINK_UDP, SEND_ADDR, &opt);
    if (!ch) {
        fprintf(stderr, "FAIL: sender open\n");
        kill(pid, SIGTERM);
        waitpid(pid, NULL, 0);
        return 1;
    }

    if (xlink_send(ch, msg, msglen) != 0) {
        fprintf(stderr, "FAIL: sender send: %s\n", xlink_errstr(ch));
        xlink_close(ch);
        kill(pid, SIGTERM);
        waitpid(pid, NULL, 0);
        return 1;
    }

    xlink_close(ch);

    int status;
    waitpid(pid, &status, 0);

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "FAIL: receiver exited with %d\n",
                WEXITSTATUS(status));
        return 1;
    }

    printf("  UDP round-trip: %zu bytes OK\n", msglen);
    return 0;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);

    int failures = 0;
    printf("=== xlink UDP test ===\n");
    failures += test_roundtrip();
    printf("=== %s ===\n", failures ? "FAILED" : "ALL PASSED");
    return failures ? 1 : 0;
}
