/*
 * TCP stress test — 1000 messages via fork-based server/client.
 */

#include "xlink.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <time.h>

#define N_MSGS   1000
static const char* ADDR = ":19997";

static long ns_diff(const struct timespec* end,
                    const struct timespec* start) {
    return (end->tv_sec - start->tv_sec) * 1000000000L
         + (end->tv_nsec - start->tv_nsec);
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);

    const char* msg = "TCP stress payload";
    size_t      msglen = strlen(msg) + 1;

    printf("=== TCP stress: %d messages ===\n", N_MSGS);

    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return 1; }

    if (pid == 0) {
        /* CHILD: server */
        xlink_opt_t opt = XLINK_OPT_DEFAULT;
        opt.flags = XLINK_SERVER;

        xlink_channel_t* ch = xlink_open(XLINK_TCP, ADDR, &opt);
        if (!ch) { fprintf(stderr, "FAIL: server open\n"); _exit(1); }

        for (int i = 0; i < N_MSGS; i++) {
            uint8_t buf[4096];
            size_t  len = sizeof(buf);
            if (xlink_recv(ch, buf, &len) != 0
                || len != msglen
                || memcmp(buf, msg, msglen) != 0) {
                fprintf(stderr, "FAIL: server recv #%d\n", i);
                xlink_close(ch);
                _exit(1);
            }
        }

        xlink_close(ch);
        _exit(0);
    }

    /* PARENT: client */
    usleep(200000);  /* give server time */

    xlink_channel_t* ch = NULL;
    for (int attempt = 0; attempt < 10; attempt++) {
        ch = xlink_open(XLINK_TCP, "127.0.0.1:19997", NULL);
        if (ch) break;
        usleep(100000);
    }
    if (!ch) { fprintf(stderr, "FAIL: client open\n"); return 1; }

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    for (int i = 0; i < N_MSGS; i++) {
        if (xlink_send(ch, msg, msglen) != 0) {
            fprintf(stderr, "FAIL: client send #%d: %s\n",
                    i, xlink_errstr(ch));
            break;
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);
    xlink_close(ch);

    int status;
    waitpid(pid, &status, 0);
    int ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;

    long elapsed_ns = ns_diff(&t1, &t0);
    printf("  passed:   %s\n", ok ? "yes" : "SERVER FAILED");
    printf("  elapsed:  %ld ms\n", elapsed_ns / 1000000);
    printf("  rate:     %ld msg/s\n",
           elapsed_ns > 0 ? (long)N_MSGS * 1000000000L / elapsed_ns : 0);

    printf("=== %s ===\n", ok ? "ALL PASSED" : "FAILED");
    return ok ? 0 : 1;
}
