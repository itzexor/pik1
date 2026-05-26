#include "logging.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static bool g_timestamps;

void pik_log_set_timestamps(bool enabled) {
    g_timestamps = enabled;
}

static void vlog_tagged(const char *tag, const char *fmt, va_list ap) {
    if (g_timestamps) {
        time_t now = time(NULL);
        struct tm tm;
        char ts[32];
        if (localtime_r(&now, &tm) && strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm))
            fprintf(stderr, "%s ", ts);
    }
    fprintf(stderr, "[%s] ", tag);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
}

void pik_log(const char *tag, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vlog_tagged(tag, fmt, ap);
    va_end(ap);
}

void pik_die(const char *tag, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vlog_tagged(tag, fmt, ap);
    va_end(ap);
    exit(1);
}

void pik_log_hex_sample(const char *tag, const char *label,
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

        pik_log(tag, "%s +%zu: %s", label, off, line);
        off += chunk;
    }
}

void pik_log_bad_frame_sample(const char *tag, const uint8_t *enc,
                              size_t enc_len) {
    size_t head = enc_len < 64 ? enc_len : 64;
    pik_log_hex_sample(tag, "badframe head", enc, head);
    if (enc_len > head) {
        pik_log(tag, "badframe tail starts at +%zu", enc_len - head);
        pik_log_hex_sample(tag, "badframe tail", enc + enc_len - head, head);
    }
}
