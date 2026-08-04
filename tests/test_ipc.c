/*
 * test_ipc.c — AF_UNIX stream socket backend tests
 *
 * Tests:
 *   1. Client-server round-trip (framed)
 *   2. Client-server multi-message
 *   3. Multi-client server broadcast + echo
 *   4. Empty recv / peek on idle
 *   5. xlink_open_url("ipc://...")
 *   6. Connect to non-existent socket → error
 *   7. Large message (64KB) round-trip
 *   8. Peek after send
 *   9. Server NONBLOCK mode
 */

#include "xlink.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <errno.h>
#include <assert.h>

#define SOCK_PATH "/tmp/xlink_test_ipc.sock"

static int checks = 0;
static int failed = 0;

#define CHK(cond, fmt, ...) do { \
    checks++; \
    if (!(cond)) { \
        fprintf(stderr, "  FAIL [%d]: " fmt "\n", checks, ##__VA_ARGS__); \
        failed++; \
    } else { \
        fprintf(stderr, "  PASS [%d]: " fmt "\n", checks, ##__VA_ARGS__); \
    } \
} while (0)

/* ─── Test 1: basic client-server round-trip ─────────── */

static void test_basic(void) {
    fprintf(stderr, "\n--- basic round-trip ---\n");
    unlink(SOCK_PATH);

    pid_t pid = fork();
    if (pid == 0) {
        /* Child: server */
        xlink_channel_t *srv = xlink_open(XLINK_IPC, SOCK_PATH,
                                          &(xlink_opt_t){.flags = XLINK_CREATE | XLINK_SERVER});
        if (!srv) { fprintf(stderr, "server open failed\n"); exit(1); }

        /* Server (multi-client) recv blocks internally with its own poll;
         * do NOT use xlink_wait() — ch->fd is the listen fd, which only
         * signals new connections, not data on existing clients. */
        char buf[256];
        size_t len = sizeof(buf);
        int r = xlink_recv(srv, buf, &len);
        if (r != 0) { fprintf(stderr, "server recv failed: %d\n", r); exit(1); }
        buf[len] = '\0';
        xlink_send(srv, buf, len);

        xlink_close(srv);
        exit(0);
    }

    /* Parent: wait for server to create socket, then connect */
    usleep(300000);
    xlink_channel_t *cli = xlink_open(XLINK_IPC, SOCK_PATH, NULL);
    CHK(cli != NULL, "client connect ok");

    const char *msg = "hello ipc";
    int r = xlink_send(cli, msg, strlen(msg));
    CHK(r == 0, "send hello (r=%d)", r);

    xlink_channel_t *chans[] = { cli };
    int idx = xlink_wait(chans, 1, 3000);
    CHK(idx == 0, "client wait for reply (idx=%d)", idx);

    char buf[256];
    size_t len = sizeof(buf);
    r = xlink_recv(cli, buf, &len);
    CHK(r == 0, "recv echo (r=%d)", r);
    buf[len] = '\0';
    CHK(strcmp(buf, "hello ipc") == 0, "echo matches (got '%s')", buf);

    xlink_close(cli);
    waitpid(pid, NULL, 0);
    unlink(SOCK_PATH);
}

/* ─── Test 2: multi-message round-trip ───────────────── */

static void test_multi_msg(void) {
    fprintf(stderr, "\n--- multi-message ---\n");
    unlink(SOCK_PATH);

    pid_t pid = fork();
    if (pid == 0) {
        xlink_channel_t *srv = xlink_open(XLINK_IPC, SOCK_PATH,
                                          &(xlink_opt_t){.flags = XLINK_CREATE | XLINK_SERVER});
        if (!srv) exit(1);

        /* Server recv blocks internally (own poll over clients) — do not
         * gate on xlink_wait() which only watches the listen fd. */
        for (int i = 0; i < 5; i++) {
            char buf[256];
            size_t len = sizeof(buf);
            if (xlink_recv(srv, buf, &len) != 0) exit(1);
            xlink_send(srv, buf, len);
        }
        xlink_close(srv);
        exit(0);
    }

    usleep(300000);
    xlink_channel_t *cli = xlink_open(XLINK_IPC, SOCK_PATH, NULL);
    CHK(cli != NULL, "multi client connect");

    for (int i = 0; i < 5; i++) {
        char msg[64];
        snprintf(msg, sizeof(msg), "msg-%d", i);
        int r = xlink_send(cli, msg, strlen(msg));
        CHK(r == 0, "send %s (r=%d)", msg, r);

        xlink_channel_t *chans[] = { cli };
        int idx = xlink_wait(chans, 1, 3000);
        CHK(idx == 0, "wait reply %d (idx=%d)", i, idx);

        char buf[256];
        size_t len = sizeof(buf);
        r = xlink_recv(cli, buf, &len);
        CHK(r == 0, "recv reply %d (r=%d)", i, r);
        buf[len] = '\0';
        CHK(strcmp(buf, msg) == 0, "reply matches %d (got '%s')", i, buf);
    }

    xlink_close(cli);
    waitpid(pid, NULL, 0);
    unlink(SOCK_PATH);
}

/* ─── Test 3: multi-client server ────────────────────── */

static void test_multi_client(void) {
    fprintf(stderr, "\n--- multi-client ---\n");
    unlink(SOCK_PATH);

    pid_t srv_pid = fork();
    if (srv_pid == 0) {
        xlink_channel_t *srv = xlink_open(XLINK_IPC, SOCK_PATH,
                                          &(xlink_opt_t){.flags = XLINK_CREATE | XLINK_SERVER});
        if (!srv) exit(1);

        xlink_channel_t *chans[] = { srv };
        /* Accept and echo 2 clients. Server recv blocks internally. */
        for (int c = 0; c < 2; c++) {
            (void)chans;
            char buf[256];
            size_t len = sizeof(buf);
            if (xlink_recv(srv, buf, &len) != 0) exit(1);
            buf[len] = '\0';
            xlink_send(srv, buf, len);
        }
        xlink_close(srv);
        exit(0);
    }

    usleep(300000);

    pid_t c1 = fork();
    if (c1 == 0) {
        xlink_channel_t *cli = xlink_open(XLINK_IPC, SOCK_PATH, NULL);
        if (!cli) exit(1);
        xlink_send(cli, "client1", 7);
        char buf[256]; size_t len = sizeof(buf);
        xlink_channel_t *chans[] = { cli };
        xlink_wait(chans, 1, 3000);
        xlink_recv(cli, buf, &len);
        buf[len] = '\0';
        xlink_close(cli);
        exit(strcmp(buf, "client1") == 0 ? 0 : 1);
    }

    pid_t c2 = fork();
    if (c2 == 0) {
        xlink_channel_t *cli = xlink_open(XLINK_IPC, SOCK_PATH, NULL);
        if (!cli) exit(1);
        xlink_send(cli, "client2", 7);
        char buf[256]; size_t len = sizeof(buf);
        xlink_channel_t *chans[] = { cli };
        xlink_wait(chans, 1, 3000);
        xlink_recv(cli, buf, &len);
        buf[len] = '\0';
        xlink_close(cli);
        exit(strcmp(buf, "client2") == 0 ? 0 : 1);
    }

    int status;
    waitpid(c1, &status, 0); CHK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "client1 ok");
    waitpid(c2, &status, 0); CHK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "client2 ok");
    waitpid(srv_pid, &status, 0); CHK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "server ok");
    unlink(SOCK_PATH);
}

/* ─── Test 4: empty recv / peek ───────────────────────── */

static void test_empty_peek(void) {
    fprintf(stderr, "\n--- empty recv / peek ---\n");
    unlink(SOCK_PATH);

    pid_t pid = fork();
    if (pid == 0) {
        xlink_channel_t *srv = xlink_open(XLINK_IPC, SOCK_PATH,
                                          &(xlink_opt_t){.flags = XLINK_CREATE | XLINK_SERVER});
        if (!srv) exit(1);
        /* Server stays open for client test */
        sleep(3);
        xlink_close(srv);
        exit(0);
    }

    usleep(300000);
    xlink_channel_t *cli = xlink_open(XLINK_IPC, SOCK_PATH, NULL);
    CHK(cli != NULL, "client connected");

    /* Peek: should show no data (no send from client) */
    size_t avail = 999;
    int r = xlink_peek(cli, &avail);
    CHK(r == 0, "peek ok");
    CHK(avail == 0, "avail == 0 on idle");

    xlink_close(cli);
    waitpid(pid, NULL, 0);
    unlink(SOCK_PATH);
}

/* ─── Test 5: ipc:// URL ──────────────────────────────── */

static void test_open_url(void) {
    fprintf(stderr, "\n--- ipc:// URL ---\n");
    unlink(SOCK_PATH);

    pid_t pid = fork();
    if (pid == 0) {
        xlink_channel_t *srv = xlink_open_url("ipc://" SOCK_PATH,
                                              &(xlink_opt_t){.flags = XLINK_CREATE | XLINK_SERVER});
        if (!srv) exit(1);

        xlink_channel_t *chans[] = { srv };
        xlink_wait(chans, 1, 3000);

        char buf[256];
        size_t len = sizeof(buf);
        xlink_recv(srv, buf, &len);
        xlink_close(srv);
        exit(0);
    }

    usleep(300000);
    xlink_channel_t *cli = xlink_open_url("ipc://" SOCK_PATH, NULL);
    CHK(cli != NULL, "open ipc:// URL");
    xlink_send(cli, "url-test", 8);

    xlink_close(cli);
    waitpid(pid, NULL, 0);
    unlink(SOCK_PATH);
}

/* ─── Test 6: connect to non-existent socket ──────────── */

static void test_no_socket(void) {
    fprintf(stderr, "\n--- no socket ---\n");
    unlink("/tmp/xlink_no_such_socket_xyz");

    xlink_channel_t *cli = xlink_open(XLINK_IPC, "/tmp/xlink_no_such_socket_xyz", NULL);
    CHK(cli == NULL, "connect to missing socket fails");
}

/* ─── Test 7: large message (64KB) ────────────────────── */

static void test_large_msg(void) {
    fprintf(stderr, "\n--- large message ---\n");
    unlink(SOCK_PATH);

    pid_t pid = fork();
    if (pid == 0) {
        xlink_channel_t *srv = xlink_open(XLINK_IPC, SOCK_PATH,
                                          &(xlink_opt_t){.flags = XLINK_CREATE | XLINK_SERVER});
        if (!srv) exit(1);

        xlink_channel_t *chans[] = { srv };
        if (xlink_wait(chans, 1, 3000) != 0) exit(1);

        char *buf = malloc(65536);
        size_t len = 65536;
        if (xlink_recv(srv, buf, &len) != 0) exit(1);
        int ok = (len == 65536);
        for (size_t i = 0; ok && i < len; i++)
            ok = (buf[i] == (char)(i & 0xff));
        free(buf);
        exit(ok ? 0 : 1);
    }

    usleep(300000);
    xlink_channel_t *cli = xlink_open(XLINK_IPC, SOCK_PATH, NULL);
    CHK(cli != NULL, "client connect for 64KB");

    char *buf = malloc(65536);
    for (size_t i = 0; i < 65536; i++)
        buf[i] = (char)(i & 0xff);

    int r = xlink_send(cli, buf, 65536);
    CHK(r == 0, "send 64KB (r=%d)", r);
    free(buf);
    xlink_close(cli);

    int status;
    waitpid(pid, &status, 0);
    CHK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "server got 64KB correctly");
    unlink(SOCK_PATH);
}

/* ─── Test 8: peek after send ─────────────────────────── */

static void test_peek_after_send(void) {
    fprintf(stderr, "\n--- peek after send ---\n");
    unlink(SOCK_PATH);

    pid_t pid = fork();
    if (pid == 0) {
        xlink_channel_t *srv = xlink_open(XLINK_IPC, SOCK_PATH,
                                          &(xlink_opt_t){.flags = XLINK_CREATE | XLINK_SERVER});
        if (!srv) exit(1);
        xlink_channel_t *chans[] = { srv };
        xlink_wait(chans, 1, 3000);

        usleep(100000);  /* let data arrive */
        size_t avail = 0;
        int r = xlink_peek(srv, &avail);
        if (r == 0 && avail > 0) exit(0);
        fprintf(stderr, "peek: r=%d avail=%zu\n", r, avail);
        exit(1);
    }

    usleep(300000);
    xlink_channel_t *cli = xlink_open(XLINK_IPC, SOCK_PATH, NULL);
    CHK(cli != NULL, "client connect");
    xlink_send(cli, "peek-data", 9);
    sleep(1);  /* let server process */

    int status;
    waitpid(pid, &status, 0);
    CHK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "peek saw data");

    xlink_close(cli);
    unlink(SOCK_PATH);
}

/* ─── Test 9: client auto-reconnect after server restart ── */

static void test_auto_reconnect(void) {
    fprintf(stderr, "\n--- client auto-reconnect ---\n");
    unlink(SOCK_PATH);

    /* Spawn a server that echoes one message then exits. */
    pid_t srv1 = fork();
    if (srv1 == 0) {
        xlink_channel_t *srv = xlink_open(XLINK_IPC, SOCK_PATH,
                                          &(xlink_opt_t){.flags = XLINK_CREATE | XLINK_SERVER});
        if (!srv) exit(1);
        char buf[32]; size_t len = sizeof(buf);
        if (xlink_recv(srv, buf, &len) != 0) exit(1);
        xlink_send(srv, buf, len);
        xlink_close(srv);
        exit(0);
    }
    usleep(300000);

    xlink_channel_t *cli = xlink_open(XLINK_IPC, SOCK_PATH, NULL);
    CHK(cli != NULL, "client open");

    /* First round-trip */
    CHK(xlink_send(cli, "hello", 5) == 0, "send first msg");
    xlink_channel_t *chans[] = { cli };
    CHK(xlink_wait(chans, 1, 3000) == 0, "wait first reply");
    char buf[32]; size_t len = sizeof(buf);
    CHK(xlink_recv(cli, buf, &len) == 0, "recv first reply");
    CHK(len == 5 && memcmp(buf, "hello", 5) == 0, "first echo matches");

    /* Server 1 exits → socket lost. */
    int st; waitpid(srv1, &st, 0);

    /* Spawn a fresh server on the same path (restart). */
    pid_t srv2 = fork();
    if (srv2 == 0) {
        xlink_channel_t *srv = xlink_open(XLINK_IPC, SOCK_PATH,
                                          &(xlink_opt_t){.flags = XLINK_CREATE | XLINK_SERVER});
        if (!srv) exit(1);
        char b[32]; size_t l = sizeof(b);
        if (xlink_recv(srv, b, &l) != 0) exit(1);
        xlink_send(srv, b, l);
        xlink_close(srv);
        exit(0);
    }
    usleep(300000);

    /* Client send on dead socket → should auto-reconnect and deliver. */
    int sent = 0;
    for (int i = 0; i < 10 && !sent; i++) {
        if (xlink_send(cli, "again", 5) == 0) { sent = 1; break; }
        usleep(200000);
    }
    CHK(sent, "client auto-reconnected on send");

    CHK(xlink_wait(chans, 1, 3000) == 0, "wait reconnect reply");
    len = sizeof(buf);
    CHK(xlink_recv(cli, buf, &len) == 0, "recv reconnect reply");
    CHK(len == 5 && memcmp(buf, "again", 5) == 0, "reconnect echo matches");

    xlink_close(cli);
    waitpid(srv2, &st, 0);
    unlink(SOCK_PATH);
}

/* ─── main ────────────────────────────────────────────── */

int main(void) {
    fprintf(stderr, "=== IPC backend tests ===\n");

    test_basic();
    test_multi_msg();
    test_multi_client();
    test_empty_peek();
    test_open_url();
    test_no_socket();
    test_large_msg();
    test_peek_after_send();
    test_auto_reconnect();

    fprintf(stderr, "\n=== RESULTS: %d/%d PASS ===\n", checks - failed, checks);
    return failed ? 1 : 0;
}
