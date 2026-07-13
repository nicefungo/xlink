/*
 * Test xlink_recv_batch() — SHM bulk receive.
 */

#include "xlink.h"
#include "shm_ipc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

static int total = 0, passed = 0;

#define CHECK(cond, msg) do {                   \
    total++;                                      \
    if (cond) passed++;                           \
    else fprintf(stderr, "FAIL: %s\n", msg);      \
} while (0)

int main(void) {
    const char *name = "/xlink_rbch";
    shm_destroy(name);

    pid_t pid = fork();
    if (pid == 0) {
        /* ── Consumer ── */
        usleep(200000);  /* wait for SHM creation + data */

        xlink_channel_t *rx = xlink_open(XLINK_SHM, name, &XLINK_OPT_DEFAULT);
        CHECK(rx != NULL, "rx open");

        char bufs[5][64] = {{0}};
        xlink_msg_t rmsgs[5];
        for (int i = 0; i < 5; i++) {
            rmsgs[i].data = bufs[i];
            rmsgs[i].len  = sizeof(bufs[i]);
        }

        int nrecv = xlink_recv_batch(rx, rmsgs, 5);
        CHECK(nrecv == 5, "recv_batch returns 5");

        for (int i = 0; i < 5; i++) {
            char exp[64];
            snprintf(exp, sizeof(exp), "rbatch_%d", i);
            CHECK(strcmp(bufs[i], exp) == 0, "message content match");
            CHECK(rmsgs[i].len == strlen(exp) + 1, "message length correct");
        }
        xlink_close(rx);
        printf("=== RESULTS: %d/%d PASS ===\n", passed, total);
        _exit(passed == total ? 0 : 1);
    }

    /* ── Producer ── */
    xlink_opt_t opt = XLINK_OPT_DEFAULT;
    opt.flags = XLINK_CREATE;

    xlink_channel_t *tx = xlink_open(XLINK_SHM, name, &opt);
    CHECK(tx != NULL, "SHM tx open");

    xlink_msg_t msgs[5];
    char payloads[5][32];
    for (int i = 0; i < 5; i++) {
        snprintf(payloads[i], sizeof(payloads[i]), "rbatch_%d", i);
        msgs[i].data = payloads[i];
        msgs[i].len  = strlen(payloads[i]) + 1;
    }

    int nsent = xlink_send_batch(tx, msgs, 5);
    CHECK(nsent == 5, "send_batch 5 => 5");
    xlink_close(tx);

    /* Wait + verify child */
    int st;
    waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st), "child exited normally");
    CHECK(WEXITSTATUS(st) == 0, "child exit 0");

    /* Error case */
    CHECK(xlink_recv_batch(NULL, NULL, 0) == -1, "NULL args => -1");

    shm_destroy(name);
    printf("=== RESULTS: %d/%d PASS ===\n", passed, total);
    return (passed == total) ? 0 : 1;
}
