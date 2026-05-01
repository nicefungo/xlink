/*
 * xlink-recv — receive from channel, write to stdout
 *
 * Usage:
 *   xlink-recv <type> <addr> [options]
 *
 * Examples:
 *   xlink-recv shm /test
 *   xlink-recv tcp 192.168.1.5:8080          # connect to server
 *   xlink-recv --server tcp :8080            # accept connections
 */

#include "xlink.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void usage(const char* prog) {
    fprintf(stderr,
        "Usage: %s [options] <type> <addr>\n"
        "Options:\n"
        "  --create     Create endpoint if absent\n"
        "  --server     Bind as server\n"
        "  --broadcast  Multiple consumers\n"
        "  --hex        Print hex dump\n"
        "\n"
        "Types: shm, pipe, tcp, udp, serial, file\n",
        prog);
    exit(1);
}

int main(int argc, char** argv) {
    xlink_opt_t opt = XLINK_OPT_DEFAULT;
    int hex_dump = 0;

    int arg = 1;
    while (arg < argc && argv[arg][0] == '-') {
        if      (strcmp(argv[arg], "--create")    == 0) opt.flags |= XLINK_CREATE;
        else if (strcmp(argv[arg], "--server")    == 0) opt.flags |= XLINK_SERVER;
        else if (strcmp(argv[arg], "--broadcast")  == 0) opt.flags |= XLINK_BROADCAST;
        else if (strcmp(argv[arg], "--hex")        == 0) hex_dump = 1;
        else usage(argv[0]);
        arg++;
    }

    if (argc - arg < 2) usage(argv[0]);

    xlink_type_t type;
    if      (strcmp(argv[arg], "shm")    == 0) type = XLINK_SHM;
    else if (strcmp(argv[arg], "pipe")   == 0) type = XLINK_PIPE;
    else if (strcmp(argv[arg], "tcp")    == 0) type = XLINK_TCP;
    else if (strcmp(argv[arg], "udp")    == 0) type = XLINK_UDP;
    else if (strcmp(argv[arg], "serial") == 0) type = XLINK_SERIAL;
    else if (strcmp(argv[arg], "file")   == 0) type = XLINK_FILE;
    else {
        fprintf(stderr, "Unknown type: %s\n", argv[arg]);
        usage(argv[0]);
    }
    arg++;

    const char* addr = argv[arg++];

    xlink_channel_t* ch = xlink_open(type, addr, &opt);
    if (!ch) {
        fprintf(stderr, "xlink_open: %s\n", xlink_errstr(ch));
        return 1;
    }

    uint8_t buf[65536];
    size_t  len;
    int     rc = 0;

    while (1) {
        len = sizeof(buf);
        rc = xlink_recv(ch, buf, &len);
        if (rc != 0) break;
        if (hex_dump) {
            fprintf(stderr, "--- recv %zu bytes ---\n", len);
            for (size_t i = 0; i < len; i++) {
                if (i > 0 && i % 16 == 0) fprintf(stderr, "\n");
                fprintf(stderr, "%02x ", buf[i]);
            }
            fprintf(stderr, "\n");
        }
        fwrite(buf, 1, len, stdout);
        fflush(stdout);
    }

    if (rc < 0) {
        fprintf(stderr, "\nxlink_recv: %s\n", xlink_errstr(ch));
    }

    xlink_close(ch);
    return rc == 0 ? 0 : 1;
}
