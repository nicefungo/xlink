/*
 * xlink-monitor — listen on a channel, print hex dump + stats
 *
 * Usage:
 *   xlink-monitor <type> <addr>
 *
 * Example:
 *   xlink-monitor shm /my_channel
 */

#include "xlink.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void usage(const char* prog) {
    fprintf(stderr,
        "Usage: %s [options] <type> <addr>\n"
        "Options:\n"
        "  --create    Create endpoint if absent\n"
        "\n"
        "Monitors a channel, prints hex dump + stats to stderr.\n",
        prog);
    exit(1);
}

static void hex_dump(const uint8_t* buf, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (i > 0 && i % 16 == 0)
            fprintf(stderr, "\n         ");
        fprintf(stderr, "%02x ", buf[i]);
    }
}

static void ascii_dump(const uint8_t* buf, size_t len) {
    fputs("  |", stderr);
    for (size_t i = 0; i < len; i++) {
        fputc(buf[i] >= 32 && buf[i] < 127 ? (char)buf[i] : '.', stderr);
    }
    fputs("|", stderr);
}

int main(int argc, char** argv) {
    xlink_opt_t opt = XLINK_OPT_DEFAULT;

    int arg = 1;
    while (arg < argc && argv[arg][0] == '-') {
        if      (strcmp(argv[arg], "--create")   == 0) opt.flags |= XLINK_CREATE;
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
        return 1;
    }
    arg++;
    const char* addr = argv[arg++];

    xlink_channel_t* ch = xlink_open(type, addr, &opt);
    if (!ch) {
        fprintf(stderr, "xlink_open: %s\n", xlink_errstr(ch));
        return 1;
    }

    fprintf(stderr, "Monitoring %s://%s\n", xlink_type_str(type), addr);
    fprintf(stderr, "----------------------------------------\n");

    uint8_t   buf[65536];
    size_t    len;
    long      msg_count = 0;
    size_t    byte_total = 0;

    while (1) {
        len = sizeof(buf);
        if (xlink_recv(ch, buf, &len) != 0) break;
        msg_count++;
        byte_total += len;

        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        fprintf(stderr, "[%ld.%03ld] msg #%ld (%zu bytes)\n",
                ts.tv_sec, ts.tv_nsec / 1000000,
                msg_count, len);
        fputs("  hex:  ", stderr);
        hex_dump(buf, len < 64 ? len : 64);
        if (len > 64) fprintf(stderr, "...");
        fputs("\n  ascii:", stderr);
        ascii_dump(buf, len < 64 ? len : 64);
        fputs("\n\n", stderr);
    }

    fprintf(stderr, "--- monitor ended ---\n");
    fprintf(stderr, "  total: %ld msgs, %zu bytes\n", msg_count, byte_total);
    xlink_close(ch);
    return 0;
}
