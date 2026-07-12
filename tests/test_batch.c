/*
 * Test xlink_send_batch() — SHM bulk send, child consumer.
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
    const char *name = "/xlink_btch";
    shm_destroy(name);

    pid_t pid = fork();
    if (pid == 0) {
        /* ── Consumer ── */
        usleep(200000);  /* wait for SHM creation */

        xlink_channel_t *rx = xlink_open(XLINK_SHM, name, &XLINK_OPT_DEFAULT);
        if (!rx) _exit(1);

        for (int i = 0; i < 10; i++) {
            char buf[64] = {0};
            size_t len = sizeof(buf);
            if (xlink_recv(rx, buf, &len) != 0) {
                xlink_close(rx);
                _exit(1);
            }
            char exp[64];
            snprintf(exp, sizeof(exp), "batch_msg_%d", i);
            if (strcmp(buf, exp) != 0) {
                xlink_close(rx);
                _exit(2);
            }
        }
        xlink_close(rx);
        _exit(0);
    }

    /* ── Producer ── */
    xlink_opt_t opt = XLINK_OPT_DEFAULT;
    opt.flags = XLINK_CREATE;

    xlink_channel_t *tx = xlink_open(XLINK_SHM, name, &opt);
    CHECK(tx != NULL, "SHM tx open");

    xlink_msg_t msgs[10];
    char payloads[10][32];
    for (int i = 0; i < 10; i++) {
        snprintf(payloads[i], sizeof(payloads[i]), "batch_msg_%d", i);
        msgs[i].data = payloads[i];
        msgs[i].len  = strlen(payloads[i]) + 1;
    }

    int nsent = xlink_send_batch(tx, msgs, 10);
    CHECK(nsent == 10, "send_batch returns 10");
    xlink_close(tx);

    /* Wait + verify child */
    int st;
    waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st), "child exited normally");
    CHECK(WEXITSTATUS(st) == 0, "child exit 0 (all 10 msgs OK)");

    /* Error case: NULL args */
    CHECK(xlink_send_batch(NULL, NULL, 0) == -1, "NULL channel returns -1");

    shm_destroy(name);
    printf("=== RESULTS: %d/%d PASS ===\n", passed, total);
    return (passed == total) ? 0 : 1;
}
