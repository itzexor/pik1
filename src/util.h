#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

int64_t pik_now_ms(void);
int pik_backoff_next(int *backoff_ms, int max_ms);

bool pik_parse_uint8(const char *s, uint8_t *out);
bool pik_parse_port(const char *s, int *out);
bool pik_parse_positive_int(const char *s, int *out);
