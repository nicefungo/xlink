#include "xlink_internal.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <termios.h>
#include <sys/stat.h>

/*
 * Serial (RS-232) backend.
 *
 * Address: "/dev/ttyX[:baud]"
 *   "/dev/ttyUSB0"         -> 9600 8N1 (default)
 *   "/dev/ttyUSB0:115200"  -> 115200 8N1
 *
 * XLINK_CREATE -> full-duplex open (O_RDWR)
 * no CREATE    -> read-only (O_RDONLY)
 *
 * Stream transport: framing auto-enabled.
 * Options: opt->serial.baud, opt->serial.bits, opt->serial.parity, opt->serial.stop
 */

static speed_t baud_to_speed(int baud) {
    switch (baud) {
        case 4800:    return B4800;
        case 9600:    return B9600;
        case 19200:   return B19200;
        case 38400:   return B38400;
        case 57600:   return B57600;
        case 115200:  return B115200;
        case 230400:  return B230400;
        default:      return B9600;
    }
}

static int serial_backend_open(xlink_channel_t* ch, const char* addr,
                               const xlink_opt_t* opt) {
    /* Parse address: split at ':' if present */
    char devpath[256];
    int  baud = 9600;

    const char* colon = strrchr(addr, ':');
    if (colon) {
        size_t len = (size_t)(colon - addr);
        if (len >= sizeof(devpath)) len = sizeof(devpath) - 1;
        memcpy(devpath, addr, len);
        devpath[len] = '\0';
        baud = (int)atol(colon + 1);
        if (baud < 1200) baud = 9600;
    } else {
        size_t len = strlen(addr);
        if (len >= sizeof(devpath)) len = sizeof(devpath) - 1;
        memcpy(devpath, addr, len);
        devpath[len] = '\0';
    }

    /* Override baud from opt if provided */
    if (opt && opt->serial.baud > 0)
        baud = opt->serial.baud;

    int flags = opt ? (int)opt->flags : 0;
    int oflags = (flags & XLINK_CREATE) ? O_RDWR : O_RDONLY;

    /* May need O_NONBLOCK if opening DCD-controlled devices */
    ch->fd = open(devpath, oflags | O_NOCTTY);
    if (ch->fd < 0) {
        snprintf(ch->errbuf, sizeof(ch->errbuf),
                 "serial: open: %s", strerror(errno));
        return -1;
    }

    /* Configure serial port */
    struct termios tio;
    memset(&tio, 0, sizeof(tio));

    if (tcgetattr(ch->fd, &tio) < 0) {
        snprintf(ch->errbuf, sizeof(ch->errbuf),
                 "serial: tcgetattr: %s", strerror(errno));
        close(ch->fd);
        ch->fd = -1;
        return -1;
    }

    speed_t speed = baud_to_speed(baud);
    if (cfsetispeed(&tio, speed) < 0 || cfsetospeed(&tio, speed) < 0) {
        snprintf(ch->errbuf, sizeof(ch->errbuf),
                 "serial: cfset*speed: %s", strerror(errno));
        close(ch->fd);
        ch->fd = -1;
        return -1;
    }

    /* Raw mode: 8N1, no flow control */
    tio.c_cflag  = CS8 | CLOCAL | CREAD;
    tio.c_iflag  = 0;
    tio.c_oflag  = 0;
    tio.c_lflag  = 0;
    tio.c_cc[VMIN]  = 1;    /* read returns when at least 1 byte */
    tio.c_cc[VTIME] = 0;    /* no timeout */

    if (opt) {
        /* Data bits */
        tio.c_cflag &= (tcflag_t)~CSIZE;
        if (opt->serial.bits == 7)      tio.c_cflag |= CS7;
        else if (opt->serial.bits == 5) tio.c_cflag |= CS5;
        else                            tio.c_cflag |= CS8;

        /* Parity */
        switch (opt->serial.parity) {
            case 'e': case 'E':
                tio.c_cflag |= PARENB;
                tio.c_cflag &= (tcflag_t)~PARODD;
                break;
            case 'o': case 'O':
                tio.c_cflag |= PARENB | PARODD;
                break;
            default:
                tio.c_cflag &= (tcflag_t)~PARENB;
                break;
        }

        /* Stop bits */
        if (opt->serial.stop >= 2)
            tio.c_cflag |= CSTOPB;
        else
            tio.c_cflag &= (tcflag_t)~CSTOPB;
    }

    /* Disable hardware flow control */
    tio.c_cflag &= (tcflag_t)~CRTSCTS;

    if (tcsetattr(ch->fd, TCSANOW, &tio) < 0) {
        snprintf(ch->errbuf, sizeof(ch->errbuf),
                 "serial: tcsetattr: %s", strerror(errno));
        close(ch->fd);
        ch->fd = -1;
        return -1;
    }

    ch->use_framing = 1;

    if (flags & XLINK_NONBLOCK) {
        int fl = fcntl(ch->fd, F_GETFL, 0);
        fcntl(ch->fd, F_SETFL, fl | O_NONBLOCK);
    }

    return 0;
}

static void serial_backend_close(xlink_channel_t* ch) {
    if (ch->fd >= 0) {
        close(ch->fd);
        ch->fd = -1;
    }
}

static int serial_backend_send(xlink_channel_t* ch, const void* data, size_t len) {
    const uint8_t* p = (const uint8_t*)data;
    size_t remain = len;
    while (remain > 0) {
        ssize_t n;
        do { n = write(ch->fd, p, remain); } while (n < 0 && errno == EINTR);
        if (n <= 0) {
            snprintf(ch->errbuf, sizeof(ch->errbuf),
                     "serial write: %s", strerror(errno));
            return -1;
        }
        p += n;
        remain -= (size_t)n;
    }
    return 0;
}

static int serial_backend_recv(xlink_channel_t* ch, void* buf, size_t* len) {
    ssize_t n;
    do { n = read(ch->fd, buf, *len); } while (n < 0 && errno == EINTR);
    if (n <= 0) {
        snprintf(ch->errbuf, sizeof(ch->errbuf),
                 "serial read: %s", n == 0 ? "no data" : strerror(errno));
        return -1;
    }
    *len = (size_t)n;
    return 0;
}

static int serial_backend_read(xlink_channel_t* ch, void* buf, size_t len, int timeout_ms) {
    struct pollfd pfd = { .fd = ch->fd, .events = POLLIN };
    int rc;
    do { rc = poll(&pfd, 1, timeout_ms); } while (rc < 0 && errno == EINTR);
    if (rc <= 0) {
        if (rc == 0) errno = ETIMEDOUT;
        return -1;
    }
    size_t n = len;
    int ret = serial_backend_recv(ch, buf, &n);
    return (ret == 0) ? (int)n : -1;
}

const xlink_backend_t xlink_serial_backend = {
    .type  = XLINK_SERIAL,
    .name  = "serial",
    .open  = serial_backend_open,
    .close = serial_backend_close,
    .send  = serial_backend_send,
    .recv  = serial_backend_recv,
    .write = NULL,
    .read  = serial_backend_read,
    .peek  = NULL,
};
