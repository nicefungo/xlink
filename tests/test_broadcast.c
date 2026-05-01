/*
 * SHM broadcast mode test.
 *
 * One writer, multiple readers. All readers must receive the same message.
 *
 * Run with: make tests && bin/tests/test_broadcast
 */

#include "xlink.h"
#include "shm_ipc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>

#define N_READERS  4
static const char* SHM_NAME = "/xlink_bcast_test";

int main(void) {
    const char* msg = "broadcast message!";
    size_t      msglen = strlen(msg) + 1;
    int         failures = 0;

    shm_destroy(SHM_NAME);

    printf("=== SHM broadcast: 1 writer → %d readers ===\n", N_READERS);

    /* Create SHM in broadcast mode with N_READERS slots */
    if (shm_create_broadcast(SHM_NAME, N_READERS) != 0) {
        fprintf(stderr, "FAIL: shm_create_broadcast\n");
        return 1;
    }

    /* Fork N_READERS child processes, each opens a reader */
    pid_t children[N_READERS];
    int   child_pipes[N_READERS][2];  /* status channel: child → parent */

    for (int i = 0; i < N_READERS; i++) {
        if (pipe(child_pipes[i]) < 0) {
            perror("pipe");
            return 1;
        }

        pid_t pid = fork();
        if (pid < 0) { perror("fork"); return 1; }

        if (pid == 0) {
            /* CHILD: open SHM reader */
            close(child_pipes[i][0]);  /* close read end */

            xlink_opt_t opt = XLINK_OPT_DEFAULT;
            opt.flags = XLINK_BROADCAST;   /* must match broadcast mode */

            xlink_channel_t* ch = xlink_open(XLINK_SHM, SHM_NAME, &opt);
            if (!ch) {
                if (write(child_pipes[i][1], "open_fail", 10) < 0) _exit(1);
                _exit(1);
            }

            uint8_t buf[4096];
            size_t  len = sizeof(buf);
            int rc = xlink_recv(ch, buf, &len);

            xlink_close(ch);

            if (rc != 0 || len != msglen
                || memcmp(buf, msg, msglen) != 0) {
                if (write(child_pipes[i][1], "recv_fail", 10) < 0) _exit(1);
                _exit(1);
            }

            if (write(child_pipes[i][1], "ok", 3) < 0) _exit(1);
            _exit(0);
        }

        children[i] = pid;
        close(child_pipes[i][1]);  /* close write end in parent */
    }

    /* PARENT: give children time to attach, then send */
    usleep(500000);

    xlink_opt_t opt = XLINK_OPT_DEFAULT;
    opt.flags = XLINK_CREATE | XLINK_BROADCAST;

    xlink_channel_t* tx = xlink_open(XLINK_SHM, SHM_NAME, &opt);
    if (!tx) {
        fprintf(stderr, "FAIL: writer open\n");
        return 1;
    }

    int rc = xlink_send(tx, msg, msglen);
    if (rc != 0) {
        fprintf(stderr, "FAIL: writer send: %s\n", xlink_errstr(tx));
        xlink_close(tx);
        return 1;
    }
    xlink_close(tx);

    /* Collect results from children */
    for (int i = 0; i < N_READERS; i++) {
        char result[32] = {0};
        ssize_t n = read(child_pipes[i][0], result, sizeof(result) - 1);
        close(child_pipes[i][0]);

        int status;
        waitpid(children[i], &status, 0);

        int child_ok = (n >= 2 && memcmp(result, "ok", 2) == 0);
        if (child_ok) {
            printf("  Reader %d: OK\n", i);
        } else {
            printf("  Reader %d: FAIL (%s)\n", i, result);
            failures++;
        }
    }

    shm_destroy(SHM_NAME);

    printf("=== %s ===\n", failures ? "FAILED" : "ALL PASSED");
    return failures ? 1 : 0;
}
