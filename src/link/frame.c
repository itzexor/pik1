#include "frame.h"
#include "nanocobs/cobs.h"

#include <string.h>

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

static uint32_t frame_crc32(const uint8_t *buf, size_t len) {
    if (!g_crc32_ready)
        crc32_init();

    uint32_t c = 0xFFFFFFFFu;
    while (len--)
        c = (c >> 8) ^ g_crc32_table[(c ^ *buf++) & 0xFF];
    return c ^ 0xFFFFFFFFu;
}

const char *pik_frame_status_text(pik_frame_status_t status) {
    switch (status) {
    case PIK_FRAME_OVERSIZED:        return "oversized frame";
    case PIK_FRAME_COBS_BAD_PAYLOAD: return "COBS bad payload";
    case PIK_FRAME_COBS_EXHAUSTED:   return "COBS exhausted";
    case PIK_FRAME_COBS_BAD_ARG:     return "COBS bad arg";
    case PIK_FRAME_SHORT:            return "short frame";
    case PIK_FRAME_CRC_MISMATCH:     return "CRC mismatch";
    case PIK_FRAME_ENCODE_FAILED:    return "encode failed";
    case PIK_FRAME_RX_OVERFLOW:      return "RX buffer full without delimiter";
    case PIK_FRAME_CALLBACK_FAILED:  return "frame callback failed";
    case PIK_FRAME_OK:
    default:                         return "ok";
    }
}

pik_frame_status_t pik_frame_encode(const uint8_t header[PIK_WIRE_HEADER_LEN],
                                    const uint8_t *payload, size_t payload_len,
                                    uint8_t *dec, size_t dec_cap,
                                    uint8_t *enc, size_t enc_cap,
                                    size_t *enc_len) {
    size_t body_len = PIK_WIRE_HEADER_LEN + payload_len;
    size_t dec_len = body_len + 4;
    if (dec_len > dec_cap)
        return PIK_FRAME_OVERSIZED;

    memcpy(dec, header, PIK_WIRE_HEADER_LEN);
    if (payload_len)
        memcpy(dec + PIK_WIRE_HEADER_LEN, payload, payload_len);

    uint32_t crc = frame_crc32(dec, body_len);
    dec[body_len + 0] = (uint8_t)crc;
    dec[body_len + 1] = (uint8_t)(crc >> 8);
    dec[body_len + 2] = (uint8_t)(crc >> 16);
    dec[body_len + 3] = (uint8_t)(crc >> 24);

    size_t out_len = 0;
    if (cobs_encode(dec, dec_len, enc, enc_cap, &out_len) != COBS_RET_SUCCESS)
        return PIK_FRAME_ENCODE_FAILED;
    *enc_len = out_len;
    return PIK_FRAME_OK;
}

pik_frame_status_t pik_frame_decode(const uint8_t *enc, size_t enc_len,
                                    size_t max_enc_len, uint8_t *dec, size_t dec_cap,
                                    pik_frame_t *frame) {
    size_t dec_len = 0;

    if (enc_len > max_enc_len)
        return PIK_FRAME_OVERSIZED;

    cobs_ret_t cr = cobs_decode(enc, enc_len, dec, dec_cap, &dec_len);
    if (cr != COBS_RET_SUCCESS) {
        if (cr == COBS_RET_ERR_BAD_PAYLOAD) return PIK_FRAME_COBS_BAD_PAYLOAD;
        if (cr == COBS_RET_ERR_EXHAUSTED) return PIK_FRAME_COBS_EXHAUSTED;
        return PIK_FRAME_COBS_BAD_ARG;
    }
    if (dec_len < PIK_WIRE_HEADER_LEN + 4)
        return PIK_FRAME_SHORT;

    size_t payload_len = dec_len - PIK_WIRE_HEADER_LEN - 4;
    uint32_t got = (uint32_t)dec[dec_len - 4]
                 | (uint32_t)dec[dec_len - 3] << 8
                 | (uint32_t)dec[dec_len - 2] << 16
                 | (uint32_t)dec[dec_len - 1] << 24;
    if (frame_crc32(dec, PIK_WIRE_HEADER_LEN + payload_len) != got)
        return PIK_FRAME_CRC_MISMATCH;

    frame->header = dec;
    frame->payload = dec + PIK_WIRE_HEADER_LEN;
    frame->payload_len = payload_len;
    return PIK_FRAME_OK;
}

pik_frame_status_t pik_frame_rx_consume(uint8_t *rxbuf, size_t *rxbuf_len,
                                        size_t rxbuf_cap,
                                        pik_frame_rx_fn fn, void *ctx) {
    while (true) {
        uint8_t *delim = memchr(rxbuf, COBS_FRAME_DELIMITER, *rxbuf_len);
        if (!delim) break;

        size_t frame_len = (size_t)(delim - rxbuf);
        if (frame_len > 0 && !fn(ctx, rxbuf, frame_len + 1))
            return PIK_FRAME_CALLBACK_FAILED;

        size_t consumed = frame_len + 1;
        *rxbuf_len -= consumed;
        memmove(rxbuf, rxbuf + consumed, *rxbuf_len);
    }

    if (*rxbuf_len >= rxbuf_cap)
        return PIK_FRAME_RX_OVERFLOW;
    return PIK_FRAME_OK;
}
