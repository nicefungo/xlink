/*
 * mock_plugin.c — a minimal loadable plugin for testing xlink_plugin_load()
 */
#include "xlink.h"

/* Internal types needed for backend definition */
#include "../src/xlink_internal.h"

/* Dummy backend — all ops return error */
static int mock_open(xlink_channel_t *ch, const char *addr,
                     const xlink_opt_t *opt) {
    (void)ch; (void)addr; (void)opt;
    return -1;
}
static void mock_close(xlink_channel_t *ch) { (void)ch; }
static int mock_send(xlink_channel_t *ch, const void *data, size_t len) {
    (void)ch; (void)data; (void)len;
    return -1;
}
static int mock_recv(xlink_channel_t *ch, void *buf, size_t *len) {
    (void)ch; (void)buf; (void)len;
    return -1;
}

static xlink_backend_t mock_backend = {
    .type  = 16,   /* XLINK_USER_BASE */
    .name  = "mock",
    .open  = mock_open,
    .close = mock_close,
    .send  = mock_send,
    .recv  = mock_recv,
    .write = NULL,
    .read  = NULL,
    .peek  = NULL,
};

/* The symbol xlink_plugin_load() looks for */
__attribute__((visibility("default")))
const xlink_plugin_t xlink_plugin_export = {
    .name        = "mock",
    .version     = "1.0-test",
    .api_version = 1,  /* XLINK_PLUGIN_API_VERSION */
    .proto       = 16,
    .backend     = &mock_backend,
    .init        = NULL,
    .fini        = NULL,
};
