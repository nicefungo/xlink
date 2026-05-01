/*
 * xlink-bridge — transparently forward between two channels
 *
 * Usage:
 *   xlink-bridge [--bidir] [--create] <typeA> <addrA> <typeB> <addrB>
 *
 * With --bidir, uses xlink_wait() to poll both directions concurrently.
 * Without --bidir, reads from A and forwards to B (unidirectional).
 */

#include "xlink.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

static void usage(const char* prog) {
    fprintf(stderr,
        "Usage: %s [options] <typeA> <addrA> <typeB> <addrB>\n"
        "Options:\n"
        "  --bidir      Forward in both directions\n"
        "  --create     Create endpoints if absent\n"
        "\nTypes: shm, pipe, tcp, udp, serial, file\n",
        prog);
    exit(1);
}

static int parse_type(const char* s, xlink_type_t* t) {
    if      (strcmp(s, "shm")    == 0) *t = XLINK_SHM;
    else if (strcmp(s, "pipe")   == 0) *t = XLINK_PIPE;
    else if (strcmp(s, "tcp")    == 0) *t = XLINK_TCP;
    else if (strcmp(s, "udp")    == 0) *t = XLINK_UDP;
    else if (strcmp(s, "serial") == 0) *t = XLINK_SERIAL;
    else if (strcmp(s, "file")   == 0) *t = XLINK_FILE;
    else return -1;
    return 0;
}

/* Forward messages from src to dst until error/EOF.
 * Returns number of forwarded messages. */
static long forward(xlink_channel_t* src, xlink_channel_t* dst,
                    const char* label) {
    uint8_t buf[65536];
    size_t  len;
    long    count = 0;

    while (1) {
        len = sizeof(buf);
        if (xlink_recv(src, buf, &len) != 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (xlink_send(dst, buf, len) != 0) {
            fprintf(stderr, "[%s] send: %s\n", label, xlink_errstr(dst));
            break;
        }
        count++;
    }
    return count;
}

/* Bidirectional: poll both channels, forward whichever is ready.
 * Returns after one of the channels disconnects. */
static void forward_bidir(xlink_channel_t* chA, xlink_channel_t* chB) {
    xlink_channel_t* chans[2] = { chA, chB };
    uint8_t buf[65536];
    size_t  len;
    long    countAB = 0, countBA = 0;

    fprintf(stderr, "Bidirectional forwarding (poll-based)...\n");

    while (1) {
        int idx = xlink_wait(chans, 2, -1);  /* wait forever */
        if (idx < 0) {
            fprintf(stderr, "xlink_wait: %s\n", strerror(errno));
            break;
        }

        xlink_channel_t* src = chans[idx];
        xlink_channel_t* dst = chans[1 - idx];
        const char* label = (idx == 0) ? "A→B" : "B→A";

        len = sizeof(buf);
        if (xlink_recv(src, buf, &len) != 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "[%s] recv: %s\n", label, xlink_errstr(src));
            break;
        }

        if (xlink_send(dst, buf, len) != 0) {
            fprintf(stderr, "[%s] send: %s\n", label, xlink_errstr(dst));
            break;
        }

        if (idx == 0) countAB++;
        else          countBA++;
    }

    fprintf(stderr, "  A→B: %ld msgs\n", countAB);
    fprintf(stderr, "  B→A: %ld msgs\n", countBA);
}

int main(int argc, char** argv) {
    xlink_opt_t optA = XLINK_OPT_DEFAULT;
    xlink_opt_t optB = XLINK_OPT_DEFAULT;
    int bidir  = 0;

    int arg = 1;
    while (arg < argc && argv[arg][0] == '-') {
        if      (strcmp(argv[arg], "--bidir")  == 0) bidir = 1;
        else if (strcmp(argv[arg], "--create") == 0) {
            optA.flags |= XLINK_CREATE;
            optB.flags |= XLINK_CREATE;
        } else usage(argv[0]);
        arg++;
    }

    if (argc - arg < 4) usage(argv[0]);

    xlink_type_t typeA, typeB;
    if (parse_type(argv[arg], &typeA) != 0) {
        fprintf(stderr, "Unknown type: %s\n", argv[arg]);
        return 1;
    }
    arg++;
    const char* addrA = argv[arg++];

    if (parse_type(argv[arg], &typeB) != 0) {
        fprintf(stderr, "Unknown type: %s\n", argv[arg]);
        return 1;
    }
    arg++;
    const char* addrB = argv[arg++];

    xlink_channel_t* chA = xlink_open(typeA, addrA, &optA);
    if (!chA) {
        fprintf(stderr, "A: xlink_open(%s): %s\n", addrA, xlink_errstr(chA));
        return 1;
    }

    xlink_channel_t* chB = xlink_open(typeB, addrB, &optB);
    if (!chB) {
        fprintf(stderr, "B: xlink_open(%s): %s\n", addrB, xlink_errstr(chB));
        xlink_close(chA);
        return 1;
    }

    if (bidir) {
        forward_bidir(chA, chB);
    } else {
        forward(chA, chB, "A→B");
    }

    xlink_close(chA);
    xlink_close(chB);
    return 0;
}
