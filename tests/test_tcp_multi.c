/*
 * TCP multi-client test: server accepts N clients, receives from all.
 * Tests that multiple clients can connect and send concurrently.
 */

#include "xlink.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>

#define PORT     ":19992"
#define ADDR     "127.0.0.1:19992"
#define NCLIENTS 3
#define MSG      "hello from client %d"
#define BUFSZ    256

static int failures = 0;
#define CHECK(cond, msg) do {                                   \
    if (!(cond)) {                                              \
        fprintf(stderr, "  FAIL: %s\n", msg);                  \
        failures++;                                             \
    } else {                                                    \
        fprintf(stderr, "  PASS: %s\n", msg);                  \
    }                                                           \
} while(0)

/* Suppress "unused result" warning for write() in child processes */
static void child_write(int fd, const void* buf, size_t n) {
    ssize_t r = write(fd, buf, n);
    (void)r;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);

    int child_pipes[NCLIENTS][2];
    for (int i = 0; i < NCLIENTS; i++) {
        if (pipe(child_pipes[i]) != 0) { perror("pipe"); return 1; }
    }

    pid_t svr_pid = fork();
    if (svr_pid == 0) {
        /* ── Server process ── */
        xlink_opt_t opt = XLINK_OPT_DEFAULT;
        opt.flags = XLINK_SERVER;
        xlink_channel_t* ch = xlink_open(XLINK_TCP, PORT, &opt);
        if (!ch) { fprintf(stderr, "S: open fail\n"); _exit(1); }

        /* Receive expected messages from all clients.
         * Multi-server recv returns messages from any ready client. */
        int found[NCLIENTS];
        memset(found, 0, sizeof(found));

        for (int i = 0; i < NCLIENTS; i++) {
            uint8_t buf[BUFSZ];
            size_t len = sizeof(buf);
            int rc = xlink_recv(ch, buf, &len);
            if (rc != 0) {
                fprintf(stderr, "S: recv #%d: %s\n", i, xlink_errstr(ch));
                _exit(1);
            }

            /* Check which client this message is from */
            int matched = 0;
            for (int j = 0; j < NCLIENTS; j++) {
                if (found[j]) continue;
                char exp[64];
                int explen = snprintf(exp, sizeof(exp), MSG, j);
                if (len == (size_t)explen + 1 && memcmp(buf, exp, len) == 0) {
                    found[j] = 1;
                    matched = 1;
                    break;
                }
            }

            if (!matched) {
                fprintf(stderr, "S: unexpected msg: %.*s\n", (int)len, (char*)buf);
                _exit(1);
            }
        }

        /* Verify all clients were heard from */
        for (int i = 0; i < NCLIENTS; i++) {
            if (!found[i]) {
                fprintf(stderr, "S: missing client %d\n", i);
                _exit(1);
            }
        }

        /* Now test broadcast: send to all connected clients */
        const char* broadcast = "broadcast msg";
        xlink_send(ch, broadcast, strlen(broadcast) + 1);

        xlink_close(ch);
        _exit(0);
    }

/* ── Parent spawns NCLIENTS client processes ── */
    pid_t child_pids[NCLIENTS];
    for (int i = 0; i < NCLIENTS; i++) {
        pid_t pid = fork();
        if (pid == 0) {
            /* Client process i */
            close(child_pipes[i][0]);

            /* Retry connect until server is ready */
            xlink_channel_t* ch = NULL;
            for (int retry = 0; retry < 20; retry++) {
                ch = xlink_open(XLINK_TCP, ADDR, NULL);
                if (ch) break;
                usleep(100000);
            }
            if (!ch) {
                child_write(child_pipes[i][1], "no", 3);
                _exit(1);
            }

            /* Send unique message */
            char msg[64];
            int msglen = snprintf(msg, sizeof(msg), MSG, i);
            if (xlink_send(ch, msg, (size_t)msglen + 1) != 0) {
                child_write(child_pipes[i][1], "send", 5);
                _exit(1);
            }

            /* Receive broadcast from server */
            uint8_t buf[BUFSZ];
            size_t len = sizeof(buf);
            if (xlink_recv(ch, buf, &len) != 0) {
                child_write(child_pipes[i][1], "recv", 5);
                _exit(1);
            }

            /* Verify broadcast content */
            const char* expected = "broadcast msg";
            if (len != strlen(expected) + 1 || memcmp(buf, expected, len) != 0) {
                child_write(child_pipes[i][1], "diff", 5);
                _exit(1);
            }

            xlink_close(ch);
            child_write(child_pipes[i][1], "ok", 3);
            _exit(0);
        }
        child_pids[i] = pid;
    }

    /* Collect client results */
    int ok_count = 0;
    for (int i = 0; i < NCLIENTS; i++) {
        close(child_pipes[i][1]);
        char result[16] = {0};
        ssize_t n = read(child_pipes[i][0], result, sizeof(result) - 1);
        if (n > 0) {
            result[n] = '\0';
            if (strcmp(result, "ok") == 0) ok_count++;
            else fprintf(stderr, "  Client %d: %s\n", i, result);
        }
        close(child_pipes[i][0]);
    }

    CHECK(ok_count == NCLIENTS, "all NCLIENTS clients received broadcast");

    /* Wait for children */
    for (int i = 0; i < NCLIENTS; i++) waitpid(child_pids[i], NULL, 0);
    waitpid(svr_pid, NULL, 0);

    fprintf(stderr, "=== %s ===\n", failures ? "FAILED" : "ALL PASSED");
    return failures ? 1 : 0;
}
