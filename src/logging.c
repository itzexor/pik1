#include "logging.h"

#include <stdio.h>

void pik_log_hex_sample(pik_log_fn log_fn, const char *label,
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

        log_fn("%s +%zu: %s", label, off, line);
        off += chunk;
    }
}
