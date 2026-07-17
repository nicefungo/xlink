/* test_batch_adaptive.c — Adaptive batching policy tests */

#include "xlink.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

static int total = 0, passed = 0;

#define CHECK(cond, msg) do {                   \
    total++;                                     \
    if (cond) passed++;                          \
    else fprintf(stderr, "FAIL: %s\n", msg);     \
} while (0)

int main(void) {
    fprintf(stderr, "=== Adaptive batch policy tests ===\n");

    fprintf(stderr, "\n--- error / edge cases ---\n");
    CHECK(xlink_set_batch_policy(NULL, &(xlink_batch_policy_t){50, 1000, 1, 0}) == -1,
          "NULL channel returns -1");
    CHECK(xlink_flush_batch(NULL) == 0, "flush(NULL) returns 0");

    fprintf(stderr, "\n--- set_batch_policy + flush basic ---\n");
    unlink("/tmp/test_bap1");
    xlink_opt_t popt = XLINK_OPT_DEFAULT;
    popt.flags = XLINK_CREATE;
    xlink_channel_t *ch = xlink_open(XLINK_PIPE, "/tmp/test_bap1", &popt);
    CHECK(ch != NULL, "pipe open");

    /* NULL policy */
    CHECK(xlink_set_batch_policy(ch, NULL) == -1, "NULL policy returns -1");

    /* flush with no policy */
    CHECK(xlink_flush_batch(ch) == 0, "flush no policy returns 0");

    /* set valid policy */
    xlink_batch_policy_t pol = {50, 100000, 5, 1};
    CHECK(xlink_set_batch_policy(ch, &pol) == 0, "valid policy set");

    /* flush empty */
    CHECK(xlink_flush_batch(ch) == 0, "flush empty batch returns 0");

    /* bad max_batch */
    pol.max_batch = 0;
    CHECK(xlink_set_batch_policy(ch, &pol) == -1, "max_batch=0 fails");
    pol.max_batch = 200;
    CHECK(xlink_set_batch_policy(ch, &pol) == -1, "max_batch>128 fails");

    xlink_close(ch);
    CHECK(1, "close with batch state (no crash)");
    unlink("/tmp/test_bap1");


    fprintf(stderr, "\n--- batch defer: below min_batch ---\n");
    unlink("/tmp/test_bap2");
    xlink_channel_t *tx = xlink_open(XLINK_PIPE, "/tmp/test_bap2", &popt);
    CHECK(tx != NULL, "tx pipe open");

    xlink_opt_t ropt = XLINK_OPT_DEFAULT;
    ropt.flags = XLINK_NONBLOCK;
    xlink_channel_t *rx = xlink_open(XLINK_PIPE, "/tmp/test_bap2", &ropt);
    CHECK(rx != NULL, "rx pipe open (nonblock)");

    /* min_batch=10, send only 3 — should NOT flush */
    xlink_batch_policy_t dpol = {50, 5000000, 10, 0}; /* 5s max_delay, non-adaptive */
    CHECK(xlink_set_batch_policy(tx, &dpol) == 0, "set defer policy");

    xlink_msg_t msgs[3];
    char bufs[3][32];
    for (int i = 0; i < 3; i++) {
        snprintf(bufs[i], sizeof(bufs[i]), "defer_%d", i);
        msgs[i] = (xlink_msg_t){bufs[i], strlen(bufs[i])};
    }
    int nsent = xlink_send_batch(tx, msgs, 3);
    CHECK(nsent == 3, "queued 3 messages");

    /* verify deferred: nothing on rx yet */
    char rbuf[64];
    size_t rlen = sizeof(rbuf);
    int rc = xlink_recv(rx, rbuf, &rlen);
    CHECK(rc == -1, "no data on rx (deferred)");

    /* manual flush */
    int nflushed = xlink_flush_batch(tx);
    CHECK(nflushed == 3, "manual flush delivers 3");

    /* now rx should get all 3 */
    for (int i = 0; i < 3; i++) {
        rlen = sizeof(rbuf);
        int rrc = xlink_recv(rx, rbuf, &rlen);
        CHECK(rrc == 0, "recv after flush OK");
        char exp[32];
        snprintf(exp, sizeof(exp), "defer_%d", i);
        CHECK(strncmp(rbuf, exp, rlen) == 0, "content match");
    }

    xlink_close(tx);
    xlink_close(rx);
    unlink("/tmp/test_bap2");


    fprintf(stderr, "\n--- batch min_batch flush ---\n");
    unlink("/tmp/test_bap3");
    tx = xlink_open(XLINK_PIPE, "/tmp/test_bap3", &popt);
    rx = xlink_open(XLINK_PIPE, "/tmp/test_bap3", &ropt);
    CHECK(tx && rx, "tx+rx open");

    /* min_batch=3, send 5 — should flush */
    xlink_batch_policy_t mpol = {50, 5000000, 3, 0};
    xlink_set_batch_policy(tx, &mpol);

    xlink_msg_t msgs2[5];
    char bufs2[5][32];
    for (int i = 0; i < 5; i++) {
        snprintf(bufs2[i], sizeof(bufs2[i]), "mb_%d", i);
        msgs2[i] = (xlink_msg_t){bufs2[i], strlen(bufs2[i])};
    }
    int nsent2 = xlink_send_batch(tx, msgs2, 5);
    CHECK(nsent2 == 5, "queued 5 messages");

    for (int i = 0; i < 5; i++) {
        rlen = sizeof(rbuf);
        CHECK(xlink_recv(rx, rbuf, &rlen) == 0, "recv after flush");
        char exp[32];
        snprintf(exp, sizeof(exp), "mb_%d", i);
        CHECK(strncmp(rbuf, exp, rlen) == 0, "content match");
    }

    xlink_close(tx);
    xlink_close(rx);
    unlink("/tmp/test_bap3");


    fprintf(stderr, "\n--- per-channel isolation ---\n");
    unlink("/tmp/test_bapA2");
    unlink("/tmp/test_bapB2");

    xlink_channel_t *cA = xlink_open(XLINK_PIPE, "/tmp/test_bapA2", &popt);
    CHECK(cA != NULL, "channel A open");

    xlink_batch_policy_t iso = {50, 5000000, 10, 0};
    xlink_set_batch_policy(cA, &iso);

    xlink_channel_t *cB = xlink_open(XLINK_PIPE, "/tmp/test_bapB2", &popt);
    CHECK(cB != NULL, "channel B open (non-batched)");

    /* Channel B: standard send (no batch state → immediate) */
    const char *bstr = "hi";
    xlink_msg_t bm = {(void*)bstr, strlen(bstr)};
    CHECK(xlink_send_batch(cB, &bm, 1) == 1, "B: immediate send OK");

    xlink_flush_batch(cA);
    xlink_close(cA);
    xlink_close(cB);
    unlink("/tmp/test_bapA2");
    unlink("/tmp/test_bapB2");
    CHECK(1, "per-channel isolation (no crash)");


    printf("=== RESULTS: %d/%d PASS ===\n", passed, total);
    return (passed == total) ? 0 : 1;
}
