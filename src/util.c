#include "util.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <time.h>

int64_t pik_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int pik_backoff_next(int *backoff_ms, int max_ms) {
    int cur = *backoff_ms;
    *backoff_ms *= 2;
    if (*backoff_ms > max_ms) *backoff_ms = max_ms;
    return cur;
}

bool pik_parse_uint8(const char *s, uint8_t *out) {
    char *end = NULL;
    errno = 0;
    unsigned long v = strtoul(s, &end, 10);
    if (errno || !end || *end || v > UINT8_MAX) return false;
    *out = (uint8_t)v;
    return true;
}

bool pik_parse_port(const char *s, int *out) {
    char *end = NULL;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno || !end || *end || v <= 0 || v > 65535) return false;
    *out = (int)v;
    return true;
}

bool pik_parse_positive_int(const char *s, int *out) {
    char *end = NULL;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno || !end || *end || v <= 0 || v > INT32_MAX) return false;
    *out = (int)v;
    return true;
}
