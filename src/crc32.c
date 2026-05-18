#include "crc32.h"

static uint32_t g_crc32_table[256];
static int g_crc32_ready;

static void crc32_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++)
            c = (c >> 1) ^ (0xEDB88320u & -(c & 1u));
        g_crc32_table[i] = c;
    }
    g_crc32_ready = 1;
}

uint32_t pik_crc32(const uint8_t *buf, size_t len) {
    if (!g_crc32_ready)
        crc32_init();

    uint32_t c = 0xFFFFFFFFu;
    while (len--)
        c = (c >> 8) ^ g_crc32_table[(c ^ *buf++) & 0xFF];
    return c ^ 0xFFFFFFFFu;
}
