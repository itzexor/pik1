#pragma once

int tty_set_byte_raw(int fd);
int tty_set_byte_raw_baud(int fd, int baud);
int tty_flush_io(int fd);
