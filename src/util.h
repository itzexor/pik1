#pragma once

#include <stddef.h>
#include <stdint.h>

typedef void (*util_log_fn)(const char *fmt, ...);

uint32_t pik_crc32(const uint8_t *buf, size_t len);
void util_log_hex_sample(util_log_fn log_fn, const char *label,
                         const uint8_t *buf, size_t len);

int tty_set_byte_raw(int fd);
int tty_set_byte_raw_baud(int fd, int baud);
