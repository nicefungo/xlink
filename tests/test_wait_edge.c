/*
 * test_wait_edge.c — xlink_wait() edge case tests.
 *
 * Covers:
 *   1. Error inputs: NULL chans, 0 channels → -2 (EINVAL)
 *   2. Error inputs: NULL element in chans[] → -2 (EINVAL)
 *   3. Poll-once: timeout=0 with data ready
 *   4. Poll-once: timeout=0 with no data → -1
 *   5. Mixed channels: SHM + pipe → wait across poll and peek paths
 *   6. Wait for SHM after pipe (data on SHM only)
 */

#include "xlink.h"
#include "shm_ipc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/wait.h>

static int failures = 0;
#define CHECK(cond, msg) do {                                    \
    if (!(cond)) {                                               \
        fprintf(stderr, "  FAIL [%d]: %s\n", __LINE__, msg);     \
        failures++;                                              \
    } else {                                                     \
        fprintf(stderr, "  PASS: %s\n", msg);                    \
    }                                                            \
} while(0)

#define PIPE_PATH  "/tmp/xlink_wait_edge_pipe"
#define SHM_NAME   "/xlink_wait_edge_shm"

static void test_error_inputs(void) {
    fprintf(stderr, "\n--- Error inputs ---\n");

    /* NULL chans */
    int rc = xlink_wait(NULL, 1, 100);
    CHECK(rc == -2, "xlink_wait(NULL, 1, 100) returns -2");

    /* 0 channels */
    xlink_channel_t* dummy = NULL;
    rc = xlink_wait(&dummy, 0, 100);
    CHECK(rc == -2, "xlink_wait(&ch, 0, 100) returns -2");

    /* NULL element in array */
    xlink_channel_t* chans[2] = { NULL, NULL };
    rc = xlink_wait(chans, 2, 100);
    CHECK(rc == -2, "xlink_wait with NULL element returns -2");

    /* Partially NULL */
    unlink(PIPE_PATH);
    xlink_opt_t opt = XLINK_OPT_DEFAULT;
    opt.flags = XLINK_CREATE;
    xlink_channel_t* ch = xlink_open(XLINK_PIPE, PIPE_PATH, &opt);
    CHECK(ch != NULL, "open pipe for partial NULL test");
    if (ch) {
        chans[0] = ch;
        chans[1] = NULL;
        rc = xlink_wait(chans, 2, 100);
        CHECK(rc == -2, "xlink_wait with one valid, one NULL returns -2");
        xlink_close(ch);
    }
    unlink(PIPE_PATH);
}

static void test_poll_once(void) {
    fprintf(stderr, "\n--- Poll-once (timeout=0) ---\n");

    unlink(PIPE_PATH);
    xlink_opt_t opt = XLINK_OPT_DEFAULT;
    opt.flags = XLINK_CREATE;
    xlink_channel_t* ch = xlink_open(XLINK_PIPE, PIPE_PATH, &opt);
    CHECK(ch != NULL, "open pipe for poll-once test");
    if (!ch) { unlink(PIPE_PATH); return; }

    xlink_channel_t* chans[1] = { ch };

    /* No data yet; timeout=0 should return immediately with -1 */
    int rc = xlink_wait(chans, 1, 0);
    CHECK(rc == -1, "timeout=0 with no data returns -1");

    /* Write data, then poll-once should find it */
    xlink_send(ch, "data", 5);
    rc = xlink_wait(chans, 1, 0);
    CHECK(rc == 0, "timeout=0 with data ready returns 0");

    /* Consume the data to clean up */
    uint8_t buf[64];
    size_t len = sizeof(buf);
    xlink_recv(ch, buf, &len);

    xlink_close(ch);
    unlink(PIPE_PATH);
}

static void test_mixed_shm_pipe(void) {
    fprintf(stderr, "\n--- Mixed SHM + pipe channels ---\n");

    shm_destroy(SHM_NAME);
    unlink(PIPE_PATH);

    /* Open SHM channel */
    xlink_opt_t shm_opt = XLINK_OPT_DEFAULT;
    shm_opt.flags = XLINK_CREATE;
    xlink_channel_t* shm_tx = xlink_open(XLINK_SHM, SHM_NAME, &shm_opt);
    CHECK(shm_tx != NULL, "open SHM sender");

    xlink_channel_t* shm_rx = xlink_open(XLINK_SHM, SHM_NAME, NULL);
    CHECK(shm_rx != NULL, "open SHM receiver");

    /* Open pipe channel */
    xlink_opt_t pipe_opt = XLINK_OPT_DEFAULT;
    pipe_opt.flags = XLINK_CREATE;
    xlink_channel_t* pipe = xlink_open(XLINK_PIPE, PIPE_PATH, &pipe_opt);
    CHECK(pipe != NULL, "open pipe");

    if (!shm_tx || !shm_rx || !pipe) {
        if (shm_tx) xlink_close(shm_tx);
        if (shm_rx) xlink_close(shm_rx);
        if (pipe) xlink_close(pipe);
        shm_destroy(SHM_NAME);
        unlink(PIPE_PATH);
        return;
    }

    /* Wait on both — no data yet */
    xlink_channel_t* chans[2] = { pipe, shm_rx };

    /* SHM channel has no fd → relies on peek() in the mixed loop */
    int rc = xlink_wait(chans, 2, 200);
    CHECK(rc == -1, "mixed wait timeout with no data returns -1");

    /* Send data to SHM → wait should return index 1 (shm_rx) */
    xlink_send(shm_tx, "shm-msg", 8);
    rc = xlink_wait(chans, 2, 1000);
    CHECK(rc == 1, "mixed wait finds data on SHM (index 1)");

    if (rc == 1) {
        uint8_t buf[128];
        size_t len = sizeof(buf);
        rc = xlink_recv(shm_rx, buf, &len);
        CHECK(rc == 0 && len == 8 && memcmp(buf, "shm-msg", 8) == 0,
              "read SHM data after mixed wait");
    }

    /* Send data to pipe → wait should return index 0 (pipe) */
    xlink_send(pipe, "pipe-data", 10);
    rc = xlink_wait(chans, 2, 1000);
    CHECK(rc == 0, "mixed wait finds data on pipe (index 0)");

    if (rc == 0) {
        uint8_t buf[128];
        size_t len = sizeof(buf);
        rc = xlink_recv(pipe, buf, &len);
        CHECK(rc == 0 && len == 10 && memcmp(buf, "pipe-data", 10) == 0,
              "read pipe data after mixed wait");
    }

    /* Both channels have data → wait returns whichever comes first */
    xlink_send(shm_tx, "second", 7);
    xlink_send(pipe, "two", 4);

    rc = xlink_wait(chans, 2, 1000);
    CHECK(rc >= 0 && rc <= 1, "mixed wait with data on both returns valid index");

    xlink_close(shm_tx);
    xlink_close(shm_rx);
    xlink_close(pipe);
    shm_destroy(SHM_NAME);
    unlink(PIPE_PATH);
}

static void test_mixed_shm_pipe_poll_once(void) {
    fprintf(stderr, "\n--- Mixed SHM + pipe poll-once (timeout=0) ---\n");

    shm_destroy(SHM_NAME);
    unlink(PIPE_PATH);

    /* Open SHM channel */
    xlink_opt_t shm_opt = XLINK_OPT_DEFAULT;
    shm_opt.flags = XLINK_CREATE;
    xlink_channel_t* shm_tx = xlink_open(XLINK_SHM, SHM_NAME, &shm_opt);
    CHECK(shm_tx != NULL, "open SHM sender for mixed poll-once");

    xlink_channel_t* shm_rx = xlink_open(XLINK_SHM, SHM_NAME, NULL);
    CHECK(shm_rx != NULL, "open SHM receiver for mixed poll-once");

    /* Open pipe channel */
    xlink_opt_t pipe_opt = XLINK_OPT_DEFAULT;
    pipe_opt.flags = XLINK_CREATE;
    xlink_channel_t* pipe = xlink_open(XLINK_PIPE, PIPE_PATH, &pipe_opt);
    CHECK(pipe != NULL, "open pipe for mixed poll-once");

    if (!shm_tx || !shm_rx || !pipe) {
        if (shm_tx) xlink_close(shm_tx);
        if (shm_rx) xlink_close(shm_rx);
        if (pipe) xlink_close(pipe);
        shm_destroy(SHM_NAME);
        unlink(PIPE_PATH);
        return;
    }

    xlink_channel_t* chans[2] = { pipe, shm_rx };

    /* No data yet; timeout=0 should poll+peek once then return -1 */
    int rc = xlink_wait(chans, 2, 0);
    CHECK(rc == -1, "mixed timeout=0 with no data returns -1");

    /* Send data to SHM; mixed timeout=0 should find it via peek */
    xlink_send(shm_tx, "poll-data", 10);
    rc = xlink_wait(chans, 2, 0);
    CHECK(rc == 1, "mixed timeout=0 finds SHM data via peek (index 1)");

    if (rc == 1) {
        uint8_t buf[128];
        size_t len = sizeof(buf);
        rc = xlink_recv(shm_rx, buf, &len);
        CHECK(rc == 0 && len == 10 && memcmp(buf, "poll-data", 10) == 0,
              "read poll-data from SHM");
    }

    /* Send data to pipe; mixed timeout=0 should find it via poll */
    xlink_send(pipe, "pipe-poll", 10);
    rc = xlink_wait(chans, 2, 0);
    CHECK(rc == 0, "mixed timeout=0 finds pipe data via poll (index 0)");

    if (rc == 0) {
        uint8_t buf[128];
        size_t len = sizeof(buf);
        rc = xlink_recv(pipe, buf, &len);
        CHECK(rc == 0 && len == 10 && memcmp(buf, "pipe-poll", 10) == 0,
              "read pipe-poll from pipe");
    }

    xlink_close(shm_tx);
    xlink_close(shm_rx);
    xlink_close(pipe);
    shm_destroy(SHM_NAME);
    unlink(PIPE_PATH);
}

static void test_pure_shm_wait(void) {
    fprintf(stderr, "\n--- Pure SHM wait (both channels fd=-1, npfd=0) ---\n");

    shm_destroy(SHM_NAME);
    shm_destroy("/xlink_wait_pure_2");

    /* Create two SHM segments */
    xlink_opt_t create_opt = XLINK_OPT_DEFAULT;
    create_opt.flags = XLINK_CREATE;
    xlink_channel_t* tx1 = xlink_open(XLINK_SHM, SHM_NAME, &create_opt);
    CHECK(tx1 != NULL, "open SHM sender 1");

    xlink_channel_t* tx2 = xlink_open(XLINK_SHM, "/xlink_wait_pure_2",
                                       &create_opt);
    CHECK(tx2 != NULL, "open SHM sender 2");

    xlink_channel_t* rx1 = xlink_open(XLINK_SHM, SHM_NAME, NULL);
    CHECK(rx1 != NULL, "open SHM receiver 1");

    xlink_channel_t* rx2 = xlink_open(XLINK_SHM, "/xlink_wait_pure_2", NULL);
    CHECK(rx2 != NULL, "open SHM receiver 2");

    if (!tx1 || !tx2 || !rx1 || !rx2) {
        if (tx1) xlink_close(tx1);
        if (tx2) xlink_close(tx2);
        if (rx1) xlink_close(rx1);
        if (rx2) xlink_close(rx2);
        shm_destroy(SHM_NAME);
        shm_destroy("/xlink_wait_pure_2");
        return;
    }

    /* Both SHM receivers have fd=-1 → npfd=0 in xlink_wait */
    xlink_channel_t* chans[2] = { rx1, rx2 };

    /* No data — timeout with 300ms */
    int rc = xlink_wait(chans, 2, 300);
    CHECK(rc == -1, "pure SHM wait timeout with no data returns -1");

    /* Send data to first SHM — wait should return index 0 */
    xlink_send(tx1, "first", 6);
    rc = xlink_wait(chans, 2, 1000);
    CHECK(rc == 0, "pure SHM wait returns index 0 when data on rx1");

    if (rc == 0) {
        uint8_t buf[128];
        size_t len = sizeof(buf);
        rc = xlink_recv(rx1, buf, &len);
        CHECK(rc == 0 && len == 6 && memcmp(buf, "first", 6) == 0,
              "read 'first' from SHM rx1");
    }

    /* Send data to second SHM — wait should return index 1 */
    xlink_send(tx2, "second", 7);
    rc = xlink_wait(chans, 2, 1000);
    CHECK(rc == 1, "pure SHM wait returns index 1 when data on rx2");

    if (rc == 1) {
        uint8_t buf[128];
        size_t len = sizeof(buf);
        rc = xlink_recv(rx2, buf, &len);
        CHECK(rc == 0 && len == 7 && memcmp(buf, "second", 7) == 0,
              "read 'second' from SHM rx2");
    }

    /* Send data to both — wait returns whichever is ready first */
    xlink_send(tx1, "both1", 6);
    xlink_send(tx2, "both2", 6);
    rc = xlink_wait(chans, 2, 1000);
    CHECK(rc >= 0 && rc <= 1,
          "pure SHM wait with data on both returns valid index 0 or 1");

    /* Consume remaining data so cleanup doesn't leak */
    {
        uint8_t buf[128];
        size_t len = sizeof(buf);
        /* Try to consume remaining data from whichever channel still has it */
        (void)xlink_recv(rx1, buf, &len);
        len = sizeof(buf);
        (void)xlink_recv(rx2, buf, &len);
    }

    /* timeout=-1 (infinite wait) with data already present in channel */
    xlink_send(tx1, "inf", 4);
    rc = xlink_wait(chans, 2, -1);
    CHECK(rc == 0, "pure SHM wait timeout=-1 with data returns 0");

    if (rc == 0) {
        uint8_t buf[128];
        size_t len = sizeof(buf);
        rc = xlink_recv(rx1, buf, &len);
        CHECK(rc == 0 && len == 4 && memcmp(buf, "inf", 4) == 0,
              "read 'inf' from SHM rx1 after timeout=-1 wait");
    }

    xlink_close(tx1);
    xlink_close(tx2);
    xlink_close(rx1);
    xlink_close(rx2);
    shm_destroy(SHM_NAME);
    shm_destroy("/xlink_wait_pure_2");
}

static void test_pure_shm_poll_once(void) {
    fprintf(stderr, "\n--- Pure SHM poll-once (timeout=0, npfd=0) ---\n");

    shm_destroy(SHM_NAME);
    shm_destroy("/xlink_wait_pure_2");

    xlink_opt_t create_opt = XLINK_OPT_DEFAULT;
    create_opt.flags = XLINK_CREATE;
    xlink_channel_t* tx1 = xlink_open(XLINK_SHM, SHM_NAME, &create_opt);
    CHECK(tx1 != NULL, "open SHM sender (pure poll-once)");

    xlink_channel_t* tx2 = xlink_open(XLINK_SHM, "/xlink_wait_pure_2",
                                       &create_opt);
    CHECK(tx2 != NULL, "open SHM sender 2 (pure poll-once)");

    xlink_channel_t* rx1 = xlink_open(XLINK_SHM, SHM_NAME, NULL);
    CHECK(rx1 != NULL, "open SHM receiver 1 (pure poll-once)");

    xlink_channel_t* rx2 = xlink_open(XLINK_SHM, "/xlink_wait_pure_2", NULL);
    CHECK(rx2 != NULL, "open SHM receiver 2 (pure poll-once)");

    if (!tx1 || !tx2 || !rx1 || !rx2) {
        if (tx1) xlink_close(tx1);
        if (tx2) xlink_close(tx2);
        if (rx1) xlink_close(rx1);
        if (rx2) xlink_close(rx2);
        shm_destroy(SHM_NAME);
        shm_destroy("/xlink_wait_pure_2");
        return;
    }

    xlink_channel_t* chans[2] = { rx1, rx2 };

    /* No data; timeout=0 → does one peek cycle, returns -1 */
    int rc = xlink_wait(chans, 2, 0);
    CHECK(rc == -1, "pure SHM timeout=0 with no data returns -1");

    /* Data on first channel; timeout=0 → finds it via peek */
    xlink_send(tx1, "once1", 6);
    rc = xlink_wait(chans, 2, 0);
    CHECK(rc == 0, "pure SHM timeout=0 finds data on rx1");

    if (rc == 0) {
        uint8_t buf[128];
        size_t len = sizeof(buf);
        rc = xlink_recv(rx1, buf, &len);
        CHECK(rc == 0 && len == 6 && memcmp(buf, "once1", 6) == 0,
              "read 'once1' from SHM rx1");
    }

    /* Data on second channel; timeout=0 → finds it */
    xlink_send(tx2, "once2", 6);
    rc = xlink_wait(chans, 2, 0);
    CHECK(rc == 1, "pure SHM timeout=0 finds data on rx2");

    if (rc == 1) {
        uint8_t buf[128];
        size_t len = sizeof(buf);
        rc = xlink_recv(rx2, buf, &len);
        CHECK(rc == 0 && len == 6 && memcmp(buf, "once2", 6) == 0,
              "read 'once2' from SHM rx2");
    }

    /* Data on both; timeout=0 → valid index returned */
    xlink_send(tx1, "duel", 5);
    xlink_send(tx2, "duel", 5);
    rc = xlink_wait(chans, 2, 0);
    CHECK(rc >= 0 && rc <= 1,
          "pure SHM timeout=0 data on both returns valid index");

    /* Cleanup remaining data */
    {
        uint8_t buf[128];
        size_t len = sizeof(buf);
        (void)xlink_recv(rx1, buf, &len);
        len = sizeof(buf);
        (void)xlink_recv(rx2, buf, &len);
    }

    xlink_close(tx1);
    xlink_close(tx2);
    xlink_close(rx1);
    xlink_close(rx2);
    shm_destroy(SHM_NAME);
    shm_destroy("/xlink_wait_pure_2");
}

static void test_mixed_infinite_wait(void) {
    fprintf(stderr, "\n--- Mixed SHM+pipe infinite wait (timeout=-1, "
                    "deadline_ms=INT64_MAX) ---\n");

    shm_destroy(SHM_NAME);
    unlink(PIPE_PATH);

    /* Create SHM sender (needs CREATE) */
    xlink_opt_t create_opt = XLINK_OPT_DEFAULT;
    create_opt.flags = XLINK_CREATE;
    xlink_channel_t* shm_tx = xlink_open(XLINK_SHM, SHM_NAME, &create_opt);
    CHECK(shm_tx != NULL, "inf: open SHM tx");

    /* Create SHM receiver */
    xlink_channel_t* shm_rx = xlink_open(XLINK_SHM, SHM_NAME, NULL);
    CHECK(shm_rx != NULL, "inf: open SHM rx");

    /* Create pipe */
    xlink_opt_t pipe_opt = XLINK_OPT_DEFAULT;
    pipe_opt.flags = XLINK_CREATE;
    xlink_channel_t* pipe = xlink_open(XLINK_PIPE, PIPE_PATH, &pipe_opt);
    CHECK(pipe != NULL, "inf: open pipe");

    if (!shm_tx || !shm_rx || !pipe) {
        if (shm_tx) xlink_close(shm_tx);
        if (shm_rx) xlink_close(shm_rx);
        if (pipe) xlink_close(pipe);
        shm_destroy(SHM_NAME);
        unlink(PIPE_PATH);
        return;
    }

    /* Fork: child sends SHM data after 200ms delay.
     * Parent does xlink_wait with timeout=-1 (infinite).
     *
     * This exercises the mixed-path loop where:
     *   npfd > 0     → pipe has fd >= 0
     *   has_peek     → SHM backend has peek
     *   timeout=-1   → deadline_ms = INT64_MAX → remain = 10
     * The loop polls(10ms) + peeks until data arrives on SHM. */
    pid_t pid = fork();
    CHECK(pid >= 0, "inf: fork");

    if (pid == 0) {
        /* Child: sleep, send data, exit */
        usleep(200000);
        int rc = xlink_send(shm_tx, "inf_sig", 8);
        if (rc != 0) _exit(1);
        _exit(0);
    }

    /* Parent: alarm kills test if child fails */
    alarm(5);
    xlink_channel_t* chans[2] = { pipe, shm_rx };
    int ready = xlink_wait(chans, 2, -1);
    alarm(0);
    CHECK(ready >= 0, "inf: wait(-1) returns ready index");
    CHECK(ready == 1, "inf: data on SHM (index 1)");

    if (ready == 1) {
        uint8_t buf[128];
        size_t len = sizeof(buf);
        int rc = xlink_recv(shm_rx, buf, &len);
        CHECK(rc == 0, "inf: recv OK");
        CHECK(len == 8 && memcmp(buf, "inf_sig", 8) == 0,
              "inf: data content 'inf_sig'");
    }

    /* Wait for child and collect status */
    int status;
    CHECK(waitpid(pid, &status, 0) == pid, "inf: waitpid");
    CHECK(WIFEXITED(status), "inf: child exited normally");
    CHECK(WEXITSTATUS(status) == 0, "inf: child exit status 0");

    xlink_close(shm_tx);
    xlink_close(shm_rx);
    xlink_close(pipe);
    shm_destroy(SHM_NAME);
    unlink(PIPE_PATH);
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);

    fprintf(stderr, "=== xlink_wait() edge case tests ===\n");

    test_error_inputs();
    test_poll_once();
    test_mixed_shm_pipe();
    test_mixed_shm_pipe_poll_once();
    test_mixed_infinite_wait();
    test_pure_shm_wait();
    test_pure_shm_poll_once();

    fprintf(stderr, "\n=== %s ===\n", failures ? "FAILED" : "ALL PASSED");
    return failures ? 1 : 0;
}
