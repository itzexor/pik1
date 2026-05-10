#include "util.h"

#include <errno.h>
#include <stdio.h>
#include <termios.h>

#ifndef CRTSCTS
#define CRTSCTS 0
#endif

static uint32_t g_crc32_table[256];
static int g_crc32_ready;

static void crc32_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++)
            c = (c >> 1) ^ (0xEDB88320u & -(c & 1u));
        g_crc32_table[i] = c;
    }
    g_crc32_ready = 1;
}

uint32_t pik_crc32(const uint8_t *buf, size_t len) {
    if (!g_crc32_ready)
        crc32_init();

    uint32_t c = 0xFFFFFFFFu;
    while (len--)
        c = (c >> 8) ^ g_crc32_table[(c ^ *buf++) & 0xFF];
    return c ^ 0xFFFFFFFFu;
}

void util_log_hex_sample(util_log_fn log_fn, const char *label,
                         const uint8_t *buf, size_t len) {
    char line[3 * 32 + 1];
    size_t off = 0;

    while (off < len) {
        size_t chunk = len - off;
        if (chunk > 32) chunk = 32;

        char *w = line;
        for (size_t i = 0; i < chunk; i++) {
            int r = snprintf(w, 4, "%02x%s", buf[off + i],
                             (i + 1 == chunk) ? "" : " ");
            if (r < 0) break;
            w += r;
        }
        *w = '\0';

        log_fn("%s +%zu: %s", label, off, line);
        off += chunk;
    }
}

static int baud_const(int baud) {
    switch (baud) {
        case 9600:   return B9600;
        case 19200:  return B19200;
        case 38400:  return B38400;
        case 57600:  return B57600;
        case 115200: return B115200;
        case 230400: return B230400;
        case 460800: return B460800;
        default:     return -1;
    }
}

static int tty_apply_byte_raw(int fd, int baud) {
    struct termios t;
    if (tcgetattr(fd, &t) < 0)
        return -1;

    t.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | INPCK | ISTRIP |
                   IGNCR | ICRNL | IXON | IXOFF | IXANY);
    t.c_oflag &= ~OPOST;
    t.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    t.c_cflag &= ~(PARENB | PARODD | CSTOPB | CSIZE | CRTSCTS);
    t.c_cflag |= CS8 | CLOCAL | CREAD;
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;

    if (baud > 0) {
        int bc = baud_const(baud);
        if (bc < 0) {
            errno = EINVAL;
            return -1;
        }
        if (cfsetispeed(&t, (speed_t)bc) < 0 || cfsetospeed(&t, (speed_t)bc) < 0)
            return -1;
    }

    return tcsetattr(fd, TCSANOW, &t);
}

int tty_set_byte_raw(int fd) {
    return tty_apply_byte_raw(fd, 0);
}

int tty_set_byte_raw_baud(int fd, int baud) {
    return tty_apply_byte_raw(fd, baud);
}
