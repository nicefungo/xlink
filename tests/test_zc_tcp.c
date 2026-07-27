/*
 * Test TCP MSG_ZEROCOPY integration.
 *
 * Tests: zc_capable, send_zc basic, multi-send, completion via error queue.
 */
#include "xlink.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/poll.h>
#include <sys/wait.h>

static int checks = 0;
static int fails = 0;

#define CHECK(cond, msg) do { \
    checks++; \
    if (!(cond)) { fprintf(stderr, "  FAIL [%d]: %s\n", __LINE__, msg); fails++; } \
    else { printf("  PASS [%d]: %s\n", __LINE__, msg); } \
} while(0)

/* ── Test 1: zc_capable for TCP ──────────────────── */

static int test_zc_capable(void) {
    printf("\n--- zc_capable ---\n");
    signal(SIGPIPE, SIG_IGN);

    /* Client mode — need a raw server to connect to */
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(19995);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    int reuse = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    if (bind(lfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "  FAIL: bind: %s\n", strerror(errno));
        close(lfd);
        return 1;
    }
    listen(lfd, 1);

    xlink_channel_t *cli = xlink_open(XLINK_TCP, "127.0.0.1:19995",
                                      &(xlink_opt_t){0});
    CHECK(cli != NULL, "open TCP client");
    if (cli) {
        int cap = xlink_zc_capable(cli);
        CHECK(cap == 1, "client mode zc_capable when connected");
        xlink_close(cli);
    }
    close(lfd);
    return (fails == 0) ? 0 : 1;
}

static int test_zc_send_basic(void) {
    printf("\n--- send_zc basic ---\n");
    signal(SIGPIPE, SIG_IGN);

    /* Raw server */
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(19908);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    int reuse = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    if (bind(lfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        CHECK(0, "bind server"); close(lfd); return 1;
    }
    listen(lfd, 1);

    xlink_channel_t *cli = xlink_open(XLINK_TCP, "127.0.0.1:19908",
                                      &(xlink_opt_t){0});
    CHECK(cli != NULL, "xlink_open TCP client");

    int cfd = accept(lfd, NULL, NULL);
    CHECK(cfd >= 0, "server accepted");

    /* zc_capable after connect */
    CHECK(xlink_zc_capable(cli) == 1, "zc_capable on client");

    /* send_zc */
    xlink_zc_buf_t buf = {
        .addr = (void *)"hello_zc_test_1234",
        .len  = 19,
        .tag  = 42,
    };
    int rc = xlink_send_zc(cli, &buf, NULL, NULL);
    CHECK(rc == 0, "xlink_send_zc succeeded");

    /* Read on server side */
    char rbuf[64] = {0};
    ssize_t rn = read(cfd, rbuf, sizeof(rbuf));
    CHECK(rn == 19, "server received correct length");
    if (rn == 19) CHECK(memcmp(rbuf, "hello_zc_test_1234", 19) == 0, "content matches");

    /* Poll completions — kernel delivers via error queue */
    usleep(50000);
    int comp = xlink_zc_poll(cli);
    CHECK(comp >= 1, "zc_poll returns completions (may be 1 or 2)");
    printf("  INFO: zc_poll returned %d completions\n", comp);

    xlink_close(cli);
    close(cfd);
    close(lfd);
    return (fails == 0) ? 0 : 1;
}

static int test_zc_multi_send(void) {
    printf("\n--- send_zc multi ---\n");
    signal(SIGPIPE, SIG_IGN);

    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(19909);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    int reuse = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    bind(lfd, (struct sockaddr*)&addr, sizeof(addr));
    listen(lfd, 1);

    xlink_channel_t *cli = xlink_open(XLINK_TCP, "127.0.0.1:19909",
                                      &(xlink_opt_t){0});
    CHECK(cli != NULL, "xlink_open");

    int cfd = accept(lfd, NULL, NULL);
    CHECK(cfd >= 0, "accepted");

    /* Send 3 messages */
    for (int i = 0; i < 3; i++) {
        char msg[16];
        snprintf(msg, sizeof(msg), "msg_%d", i);
        xlink_zc_buf_t buf = { .addr = msg, .len = 5, .tag = (uint64_t)(100 + i) };
        int rc = xlink_send_zc(cli, &buf, NULL, NULL);
        CHECK(rc == 0, "send_zc ok");
    }

    /* Verify server received — raw sendmsg(MSG_ZEROCOPY) sends
     * data directly without framing. Each msg is 5 bytes. Total: 15. */
    size_t total = 0;
    char rbuf[64] = {0};
    while (total < 15) {
        ssize_t rn = read(cfd, rbuf + total, sizeof(rbuf) - total);
        if (rn <= 0) break;
        total += (size_t)rn;
    }
    printf("  server total recv: %zu bytes\n", total);
    CHECK(total == 15, "server received 15 bytes (3 raw messages)");
    /* Content: "msg_0msg_1msg_2" (concatenated) */
    CHECK(memcmp(rbuf, "msg_0msg_1msg_2", 15) == 0, "multi content correct");

    /* Poll completions */
    usleep(100000);
    int comp = xlink_zc_poll(cli);
    printf("  INFO: zc_poll returned %d completions\n", comp);
    CHECK(comp >= 0, "zc_poll ok");

    xlink_close(cli);
    close(cfd);
    close(lfd);
    return (fails == 0) ? 0 : 1;
}

static int test_zc_errors(void) {
    printf("\n--- error paths ---\n");

    CHECK(xlink_send_zc(NULL, &(xlink_zc_buf_t){"x", 1, 0, 1},
                       NULL, NULL) == -1, "send_zc NULL ch returns -1");

    CHECK(xlink_zc_poll(NULL) == -1, "zc_poll NULL returns -1");

    /* Create temp server for error test */
    int lfd2 = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr2 = {0};
    addr2.sin_family = AF_INET;
    addr2.sin_port = htons(19903);
    addr2.sin_addr.s_addr = inet_addr("127.0.0.1");
    int reuse2 = 1;
    setsockopt(lfd2, SOL_SOCKET, SO_REUSEADDR, &reuse2, sizeof(reuse2));
    bind(lfd2, (struct sockaddr*)&addr2, sizeof(addr2));
    listen(lfd2, 1);

    xlink_channel_t *cli = xlink_open(XLINK_TCP, "127.0.0.1:19903",
                                      &(xlink_opt_t){0});
    if (cli) {
        CHECK(xlink_send_zc(cli, &(xlink_zc_buf_t){NULL, 0, 0, 0},
                           NULL, NULL) == -1, "send_zc empty buf returns -1");
        xlink_close(cli);
    }
    close(lfd2);

    return (fails == 0) ? 0 : 1;
}

static int test_zc_notify_fd(void) {
    printf("\n--- zc_notify_fd for TCP ---\n");
    signal(SIGPIPE, SIG_IGN);

    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(19910);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    int reuse = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    bind(lfd, (struct sockaddr*)&addr, sizeof(addr));
    listen(lfd, 1);

    xlink_channel_t *cli = xlink_open(XLINK_TCP, "127.0.0.1:19910",
                                      &(xlink_opt_t){0});
    CHECK(cli != NULL, "xlink_open");

    int notify_fd = xlink_zc_notify_fd(cli);
    CHECK(notify_fd >= 0, "zc_notify_fd returns valid fd");

    int cfd = accept(lfd, NULL, NULL);
    CHECK(cfd >= 0, "accepted");

    xlink_zc_buf_t buf = { .addr = (void *)"notify", .len = 6, .tag = 1 };
    int rc = xlink_send_zc(cli, &buf, NULL, NULL);
    CHECK(rc == 0, "send_zc");

    /* eventfd should be readable after completion is queued */
    usleep(50000);
    struct pollfd pfd = { .fd = notify_fd, .events = POLLIN };
    int prc = poll(&pfd, 1, 500);
    CHECK(prc > 0, "zc_notify_fd readable after send_zc");

    xlink_close(cli);
    close(cfd);
    close(lfd);
    return (fails == 0) ? 0 : 1;
}

int main(void) {
    setbuf(stdout, NULL);  /* unbuffered for diagnostic output */
    printf("=== TCP MSG_ZEROCOPY tests ===\n");

    int r = 0;
    r |= test_zc_capable();
    r |= test_zc_send_basic();
    r |= test_zc_multi_send();
    r |= test_zc_errors();
    r |= test_zc_notify_fd();

    printf("\n=== RESULTS: %d checks, %d failures ===\n", checks, fails);
    return r;
}
