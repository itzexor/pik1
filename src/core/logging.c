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

_Noreturn void pik_die(const char *tag, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vlog_tagged(tag, fmt, ap);
    va_end(ap);
    exit(1);
}
