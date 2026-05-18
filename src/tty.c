#include "tty.h"

#include <errno.h>
#include <termios.h>

#ifndef CRTSCTS
#define CRTSCTS 0
#endif

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
