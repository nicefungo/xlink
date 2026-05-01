/*
 * xlink-send — read from stdin, send to channel
 *
 * Usage:
 *   xlink-send <type> <addr> [options]
 *
 * Examples:
 *   echo "hello" | xlink-send shm /test
 *   xlink-send --create shm /test    # interactive stdin
 *   xlink-send --server tcp :8080    # TCP server, echo stdin → all clients
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
        "  --create     Create endpoint if absent (SHM, PIPE)\n"
        "  --server     Bind as server (TCP, PIPE)\n"
        "  --broadcast  Multiple consumers (SHM)\n"
        "\n"
        "Types: shm, pipe, tcp, udp, serial, file\n"
        "\n"
        "Sends stdin line by line to the channel.\n"
        "With no stdin (interactive), reads line by line from terminal.\n",
        prog);
    exit(1);
}

int main(int argc, char** argv) {
    xlink_opt_t opt = XLINK_OPT_DEFAULT;

    int arg = 1;
    while (arg < argc && argv[arg][0] == '-') {
        if      (strcmp(argv[arg], "--create")   == 0) opt.flags |= XLINK_CREATE;
        else if (strcmp(argv[arg], "--server")   == 0) opt.flags |= XLINK_SERVER;
        else if (strcmp(argv[arg], "--broadcast") == 0) opt.flags |= XLINK_BROADCAST;
        else usage(argv[0]);
        arg++;
    }

    if (argc - arg < 2) usage(argv[0]);

    /* Parse type */
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

    /* Open channel */
    xlink_channel_t* ch = xlink_open(type, addr, &opt);
    if (!ch) {
        fprintf(stderr, "xlink_open: %s\n", xlink_errstr(ch));
        return 1;
    }

    /* Read stdin, send lines */
    char   buf[65536];
    size_t nread;

    while ((nread = fread(buf, 1, sizeof(buf), stdin)) > 0) {
        if (xlink_send(ch, buf, nread) != 0) {
            fprintf(stderr, "xlink_send: %s\n", xlink_errstr(ch));
            xlink_close(ch);
            return 1;
        }
    }

    if (feof(stdin) && nread == 0 && isatty(STDIN_FILENO)) {
        /* Interactive mode — read from terminal */
        while (fgets(buf, sizeof(buf), stdin)) {
            size_t len = strlen(buf);
            if (xlink_send(ch, buf, len) != 0) {
                fprintf(stderr, "xlink_send: %s\n", xlink_errstr(ch));
                break;
            }
        }
    }

    xlink_close(ch);
    return 0;
}
