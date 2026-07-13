#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

void pik_log_set_timestamps(bool enabled);
void pik_log(const char *tag, const char *fmt, ...);
void pik_die(const char *tag, const char *fmt, ...);

void pik_log_hex_sample(const char *tag, const char *label,
                        const uint8_t *buf, size_t len);
void pik_log_bad_frame_sample(const char *tag, const uint8_t *enc,
                              size_t enc_len);
