#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

int64_t pik_now_ms(void);
int pik_backoff_next(int *backoff_ms, int max_ms);
uint32_t pik_session_id(int64_t now);

bool pik_parse_uint8(const char *s, uint8_t *out);
bool pik_parse_port(const char *s, int *out);
bool pik_parse_positive_int(const char *s, int *out);

static inline void pik_put_u32le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static inline uint32_t pik_get_u32le(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
