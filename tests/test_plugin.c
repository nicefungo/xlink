/* test_plugin.c — plugin registry and xlink_open_url() tests */

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include "xlink.h"
#include "../src/xlink_internal.h"

#define PASS(fmt, ...) printf("  PASS: " fmt "\n", ##__VA_ARGS__)
#define FAIL(fmt, ...) do { \
    printf("  FAIL: " fmt "\n", ##__VA_ARGS__); \
    return 1; \
} while (0)

static int npass = 0, nfail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { PASS(msg); npass++; } \
    else { FAIL(msg); nfail++; } \
} while (0)

int main(void) {
    printf("=== Plugin registry tests ===\n\n");

    /* Force plugin init — needed because we're calling plugin API directly
     * without going through xlink_open() first. */
    xlink_plugins_init();

    /* ─── Built-in plugins are auto-registered ─── */
    printf("--- Built-in plugin registration ---\n");
    const xlink_plugin_t *pl;

    pl = xlink_plugin_find("shm");
    CHECK(pl != NULL, "find 'shm' returns non-NULL");
    CHECK(pl->proto == XLINK_SHM, "shm proto == XLINK_SHM");
    CHECK(pl->backend != NULL, "shm backend is set");

    pl = xlink_plugin_find("pipe");
    CHECK(pl != NULL, "find 'pipe' returns non-NULL");
    CHECK(pl->backend != NULL, "pipe backend is set");

    pl = xlink_plugin_find("tcp");
    CHECK(pl != NULL, "find 'tcp' returns non-NULL");

    pl = xlink_plugin_find("udp");
    CHECK(pl != NULL, "find 'udp' returns non-NULL");

    pl = xlink_plugin_find("serial");
    CHECK(pl != NULL, "find 'serial' returns non-NULL");

    pl = xlink_plugin_find("file");
    CHECK(pl != NULL, "find 'file' returns non-NULL");

    pl = xlink_plugin_find("nonexistent");
    CHECK(pl == NULL, "find 'nonexistent' returns NULL");

    /* ─── Find by type ─── */
    printf("\n--- Find by type ---\n");
    pl = xlink_plugin_find_by_type(XLINK_SHM);
    CHECK(pl != NULL && strcmp(pl->name, "shm") == 0,
          "find_by_type SHM → 'shm'");
    pl = xlink_plugin_find_by_type(XLINK_TCP);
    CHECK(pl != NULL && strcmp(pl->name, "tcp") == 0,
          "find_by_type TCP → 'tcp'");
    pl = xlink_plugin_find_by_type((xlink_type_t)255);
    CHECK(pl == NULL, "find_by_type 255 returns NULL");

    /* ─── Register a custom plugin ─── */
    printf("\n--- Custom plugin registration ---\n");

    xlink_plugin_t custom = {
        .name = "mock",
        .version = "0.1",
        .api_version = XLINK_PLUGIN_API_VERSION,
        .proto = 16,  /* XLINK_USER_BASE */
        .backend = NULL,   /* No real backend needed for this test */
        .init = NULL,
        .fini = NULL,
    };

    int rc = xlink_plugin_register(&custom);
    CHECK(rc == 0, "register 'mock' succeeds");

    pl = xlink_plugin_find("mock");
    CHECK(pl != NULL, "find 'mock' returns non-NULL");
    CHECK(pl->proto == 16, "mock proto == 16");

    /* Duplicate register should fail */
    rc = xlink_plugin_register(&custom);
    CHECK(rc == -1, "register duplicate 'mock' fails");

    /* Unregister */
    rc = xlink_plugin_unregister("mock");
    CHECK(rc == 0, "unregister 'mock' succeeds");
    pl = xlink_plugin_find("mock");
    CHECK(pl == NULL, "find 'mock' after unregister returns NULL");

    /* Unregister nonexistent */
    rc = xlink_plugin_unregister("mock");
    CHECK(rc == -1, "unregister nonexistent fails");

    /* ─── xlink_open_url() ─── */
    printf("\n--- xlink_open_url() ---\n");

    /* SHM via URL */
    xlink_opt_t opt = {
        .flags = XLINK_CREATE,
        .buf_size = 0,
        .timeout_ms = -1,
        .shm = {0}
    };
    xlink_channel_t *ch = xlink_open_url("shm://url_test_chan", &opt);
    CHECK(ch != NULL, "xlink_open_url('shm://...') returns non-NULL");
    if (ch) {
        CHECK(ch->backend != NULL, "url SHM channel has backend");
        CHECK(strcmp(ch->backend->name, "shm") == 0,
              "url SHM backend name is 'shm'");

        /* Round-trip via URL channel */
        const char *msg = "URL test OK";
        rc = xlink_send(ch, msg, strlen(msg) + 1);
        CHECK(rc == 0, "url SHM send succeeds");

        xlink_channel_t *rx = xlink_open(XLINK_SHM, "url_test_chan",
                                         &XLINK_OPT_DEFAULT);
        CHECK(rx != NULL, "read side open succeeds");
        if (rx) {
            size_t len = 256;
            char buf[256];
            rc = xlink_recv(rx, buf, &len);
            CHECK(rc == 0, "url SHM recv succeeds");
            CHECK(strcmp(buf, "URL test OK") == 0, "url SHM content matches");
            xlink_close(rx);
        }

        xlink_close(ch);
    }

    /* Invalid URL */
    ch = xlink_open_url("invalid_without_scheme", NULL);
    CHECK(ch == NULL, "xlink_open_url(no-scheme) returns NULL");

    ch = xlink_open_url("unknown://foo", NULL);
    CHECK(ch == NULL, "xlink_open_url(unknown-scheme) returns NULL");

    ch = xlink_open_url("://noproto", NULL);
    CHECK(ch == NULL, "xlink_open_url(empty-scheme) returns NULL");

    /* ─── xlink_plugin_count() ─── */
    printf("\n--- Plugin count ---\n");
    size_t cnt = xlink_plugin_count();
    CHECK(cnt == 6, "plugin_count() == 6 (built-ins only)");

    /* ─── Dynamic .so loading ─── */
    printf("\n--- Dynamic .so loading ---\n");

#ifdef __linux__
    /* Load the mock plugin from .so (build it first) */
    int ld_rc = xlink_plugin_load("bin/tests/mock_plugin.so");
    CHECK(ld_rc == 0, "xlink_plugin_load('mock_plugin.so') succeeds");

    pl = xlink_plugin_find("mock");
    CHECK(pl != NULL, "find 'mock' after load returns non-NULL");
    CHECK(pl->proto == 16, "loaded mock proto == 16");
    CHECK(pl->backend != NULL, "loaded mock backend is set");

    cnt = xlink_plugin_count();
    CHECK(cnt == 7, "plugin_count() == 7 after loading mock");

    /* Load nonexistent .so */
    ld_rc = xlink_plugin_load("bin/tests/nonexistent.so");
    CHECK(ld_rc == -1, "xlink_plugin_load(nonexistent) fails");

    /* Unregister after load */
    int urc = xlink_plugin_unregister("mock");
    CHECK(urc == 0, "unregister loaded 'mock' succeeds");
    cnt = xlink_plugin_count();
    CHECK(cnt == 6, "plugin_count() == 6 after unregister");
#else
    printf("  SKIP: .so loading not available on this platform\n");
#endif

    /* ─── xlink_type_str() still works ─── */
    printf("\n--- xlink_type_str() ---\n");
    const char *ts;
    ts = xlink_type_str(XLINK_SHM);
    CHECK(ts != NULL && strcmp(ts, "shm") == 0,
          "type_str(SHM) == 'shm'");
    ts = xlink_type_str(XLINK_TCP);
    CHECK(ts != NULL && strcmp(ts, "tcp") == 0,
          "type_str(TCP) == 'tcp'");
    ts = xlink_type_str((xlink_type_t)255);
    CHECK(ts != NULL && strcmp(ts, "unknown") == 0,
          "type_str(255) == 'unknown'");

    printf("\n=== RESULTS: %d checks, %d failures ===\n", npass + nfail, nfail);
    return nfail > 0 ? 1 : 0;
}
