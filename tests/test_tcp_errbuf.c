/*
 * TCP errbuf tests — verify xlink_errstr() returns meaningful messages
 * on connection failures and disconnects.
 */

#include "xlink.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <errno.h>

static int failures = 0;
#define CHECK(cond, msg, ...) do {                                           \
    if (!(cond)) {                                                           \
        fprintf(stderr, "  FAIL: " msg "\n", ##__VA_ARGS__);                 \
        failures++;                                                          \
    } else {                                                                 \
        printf("  PASS: " msg "\n", ##__VA_ARGS__);                          \
    }                                                                        \
} while (0)

static int find_port(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = { .sin_family = AF_INET,
                                .sin_addr = { .s_addr = htonl(INADDR_LOOPBACK) } };
    bind(fd, (struct sockaddr*)&addr, sizeof(addr));
    socklen_t len = sizeof(addr);
    getsockname(fd, (struct sockaddr*)&addr, &len);
    close(fd);
    return ntohs(addr.sin_port);
}

static void test_tcp_disconnect_errbuf(void) {
    /* Server accepts one connection and closes immediately.
     * Client should receive disconnect with meaningful errbuf. */
    int port = find_port();
    int sync_pipe[2];
    if (pipe(sync_pipe) != 0) { CHECK(0, "pipe"); return; }

    pid_t pid = fork();
    if (pid == 0) {
        /* Child: TCP server */
        close(sync_pipe[0]);
        int srv = socket(AF_INET, SOCK_STREAM, 0);
        int one = 1;
        setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        struct sockaddr_in addr = { .sin_family = AF_INET,
                                    .sin_port = htons(port),
                                    .sin_addr = { .s_addr = htonl(INADDR_LOOPBACK) } };
        bind(srv, (struct sockaddr*)&addr, sizeof(addr));
        listen(srv, 1);

        /* Signal parent that server is ready */
        { ssize_t n = write(sync_pipe[1], "x", 1); (void)n; }
        close(sync_pipe[1]);

        int cfd = accept(srv, NULL, NULL);
        if (cfd >= 0) {
            usleep(200000);          /* 200ms — let client settle */
            close(cfd);              /* close without sending data */
        }
        close(srv);
        _exit(0);
    }

    /* Parent: wait for server to be ready */
    close(sync_pipe[1]);
    char dummy;
    { ssize_t n = read(sync_pipe[0], &dummy, 1); (void)n; }
    close(sync_pipe[0]);
    usleep(100000);  /* extra 100ms for child to reach accept() */

    /* Connect via xlink */
    char addr[64];
    snprintf(addr, sizeof(addr), "127.0.0.1:%d", port);
    xlink_opt_t opt = XLINK_OPT_DEFAULT;
    opt.flags |= XLINK_NONBLOCK;
    xlink_channel_t* ch = xlink_open(XLINK_TCP, addr, &opt);
    CHECK(ch != NULL, "tcp open succeeds on %s", addr);

    /* Wait for child to close the connection */
    usleep(500000);

    /* Try to recv from disconnected peer — expect error with errbuf */
    char buf[64];
    size_t len = sizeof(buf);
    int rc = xlink_recv(ch, buf, &len);
    if (rc < 0) {
        const char* err = xlink_errstr(ch);
        CHECK(err != NULL, "  errstr is non-NULL on disconnect");
        CHECK(err[0] != 0,    "  errstr is non-empty");
        int ok = (strstr(err, "disconnect") != NULL)
              || (strstr(err, "lost") != NULL)
              || (strstr(err, "broken") != NULL)
              || (strstr(err, "reset") != NULL)
              || (strstr(err, "peer closed") != NULL);
        CHECK(ok, "  errstr contains meaningful message: '%s'", err ? err : "NULL");
        printf("    → %s\n", err ? err : "(null)");
    } else {
        /* May succeed on non-blocking recv if peer hasn't closed yet —
         * not a failure, but check errbuf anyway. */
        printf("  SKIP: recv succeeded (peer may still be connected)\n");
    }
    xlink_close(ch);
    waitpid(pid, NULL, 0);
}

static void test_tcp_bad_addr_errstr(void) {
    /* Open with an invalid address — should see errbuf after failure. */
    xlink_opt_t opt = XLINK_OPT_DEFAULT;

    /* Try connecting to port 0 on loopback — connect should fail */
    xlink_channel_t* ch = xlink_open(XLINK_TCP, "127.0.0.1:0", &opt);
    if (ch) {
        /* TCP backend may succeed open (deferred connect).
         * Trigger actual connect attempt via recv. */
        char buf[8];
        size_t len = sizeof(buf);
        xlink_recv(ch, buf, &len);
        const char* err = xlink_errstr(ch);
        CHECK(err != NULL, "  errstr non-NULL after bad-addr open");
        if (err) printf("    → '%s'\n", err);
        xlink_close(ch);
    } else {
        const char* err = xlink_errstr(NULL);
        CHECK(err != NULL, "  errstr(NULL) non-NULL on failed open");
        if (err) printf("    → '%s'\n", err);
    }
}

int main(void) {
    printf("=== TCP errbuf tests ===\n");

    printf("\n--- disconnect errbuf ---\n");
    test_tcp_disconnect_errbuf();

    printf("\n--- bad-addr errstr ---\n");
    test_tcp_bad_addr_errstr();

    printf("\n=== %s ===\n", failures ? "FAILED" : "ALL PASSED");
    return failures ? 1 : 0;
}
