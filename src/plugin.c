/*
 * plugin.c — xlink plugin registry
 *
 * Manages a hash table of registered plugins.  Built-in backends
 * are auto-registered via xlink_plugins_init() at first use.
 *
 * Threading: NOT thread-safe (xlink targets single-threaded apps).
 * The registry is initialized lazily on first register/lookup.
 */
#include "xlink_internal.h"
#include <stdlib.h>
#include <string.h>

/* ─── Hash table ──────────────────────────────────────── */

#define PLUGIN_HASH_BUCKETS  32
#define XLINK_USER_BASE      16    /* user plugin IDs start here */

typedef struct plugin_entry {
    xlink_plugin_t          *plugin;
    struct plugin_entry     *next;
} plugin_entry_t;

static plugin_entry_t *plugin_table[PLUGIN_HASH_BUCKETS];
static int             plugin_inited = 0;

/* djb2 hash */
static unsigned hash_str(const char *s) {
    unsigned h = 5381;
    int c;
    while ((c = *s++))
        h = ((h << 5) + h) + (unsigned)c;
    return h % PLUGIN_HASH_BUCKETS;
}

/* ─── Built-in backend registration ───────────────────── */

static int  builtin_init_dummy(void) { return 0; }
static void builtin_fini_dummy(void) {}

#define BUILTIN_PLUGIN(_type, _name, _be)              \
    { .name = (_name), .version = "1.0",               \
      .api_version = 1, .proto = (_type),              \
      .backend = (_be), .init = builtin_init_dummy,    \
      .fini = builtin_fini_dummy, {0} }

static const xlink_plugin_t builtin_plugins[] = {
    BUILTIN_PLUGIN(XLINK_SHM,    "shm",    &xlink_shm_backend),
    BUILTIN_PLUGIN(XLINK_PIPE,   "pipe",   &xlink_pipe_backend),
    BUILTIN_PLUGIN(XLINK_TCP,    "tcp",    &xlink_tcp_backend),
    BUILTIN_PLUGIN(XLINK_UDP,    "udp",    &xlink_udp_backend),
    BUILTIN_PLUGIN(XLINK_SERIAL, "serial", &xlink_serial_backend),
    BUILTIN_PLUGIN(XLINK_FILE,   "file",   &xlink_file_backend),
};

void xlink_plugins_init(void) {
    if (plugin_inited) return;
    plugin_inited = 1;

    for (size_t i = 0; i < sizeof(builtin_plugins) / sizeof(builtin_plugins[0]); i++)
        xlink_plugin_register(&builtin_plugins[i]);
}

/* ─── Plugin manager API ──────────────────────────────── */

int xlink_plugin_register(const xlink_plugin_t *plugin) {
    if (!plugin || !plugin->name)
        return -1;

    /* Check for duplicate */
    if (xlink_plugin_find(plugin->name))
        return -1;

    unsigned bucket = hash_str(plugin->name);

    plugin_entry_t *e = calloc(1, sizeof(*e));
    if (!e) return -1;

    /* Plugin struct may be static — make a copy so we own it */
    xlink_plugin_t *copy = calloc(1, sizeof(*copy));
    if (!copy) { free(e); return -1; }
    *copy = *plugin;

    e->plugin = copy;
    e->next   = plugin_table[bucket];
    plugin_table[bucket] = e;

    if (copy->init)
        copy->init();

    return 0;
}

int xlink_plugin_unregister(const char *name) {
    if (!name) return -1;

    unsigned bucket = hash_str(name);
    plugin_entry_t *prev = NULL;
    plugin_entry_t *e = plugin_table[bucket];

    while (e) {
        if (strcmp(e->plugin->name, name) == 0) {
            if (e->plugin->fini)
                e->plugin->fini();
            free(e->plugin);
            if (prev)
                prev->next = e->next;
            else
                plugin_table[bucket] = e->next;
            free(e);
            return 0;
        }
        prev = e;
        e = e->next;
    }
    return -1;
}

const xlink_plugin_t *xlink_plugin_find(const char *name) {
    if (!name) return NULL;

    unsigned bucket = hash_str(name);
    for (plugin_entry_t *e = plugin_table[bucket]; e; e = e->next) {
        if (strcmp(e->plugin->name, name) == 0)
            return e->plugin;
    }
    return NULL;
}

const xlink_plugin_t *xlink_plugin_find_by_type(xlink_type_t type) {
    for (int i = 0; i < PLUGIN_HASH_BUCKETS; i++) {
        for (plugin_entry_t *e = plugin_table[i]; e; e = e->next) {
            if (e->plugin->proto == type)
                return e->plugin;
        }
    }
    return NULL;
}
