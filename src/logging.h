#pragma once

#include <stddef.h>
#include <stdint.h>

typedef void (*pik_log_fn)(const char *fmt, ...);

void pik_log_hex_sample(pik_log_fn log_fn, const char *label,
                        const uint8_t *buf, size_t len);
