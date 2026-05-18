#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    PIK_FRAME_OK = 0,
    PIK_FRAME_OVERSIZED,
    PIK_FRAME_COBS_BAD_PAYLOAD,
    PIK_FRAME_COBS_EXHAUSTED,
    PIK_FRAME_COBS_BAD_ARG,
    PIK_FRAME_SHORT,
    PIK_FRAME_CRC_MISMATCH,
    PIK_FRAME_ENCODE_FAILED,
    PIK_FRAME_RX_OVERFLOW,
    PIK_FRAME_CALLBACK_FAILED,
} pik_frame_status_t;

typedef struct {
    const uint8_t *header;
    size_t header_len;
    const uint8_t *payload;
    size_t payload_len;
    size_t decoded_len;
} pik_frame_t;

typedef bool (*pik_frame_rx_fn)(void *ctx, const uint8_t *enc, size_t enc_len);

const char *pik_frame_status_text(pik_frame_status_t status);

pik_frame_status_t pik_frame_encode(const uint8_t *header, size_t header_len,
                                    const uint8_t *payload, size_t payload_len,
                                    uint8_t *dec, size_t dec_cap,
                                    uint8_t *enc, size_t enc_cap,
                                    size_t *enc_len);

pik_frame_status_t pik_frame_decode(const uint8_t *enc, size_t enc_len,
                                    size_t max_enc_len, size_t header_len,
                                    uint8_t *dec, size_t dec_cap,
                                    pik_frame_t *frame);

pik_frame_status_t pik_frame_rx_consume(uint8_t *rxbuf, size_t *rxbuf_len,
                                        size_t rxbuf_cap,
                                        pik_frame_rx_fn fn, void *ctx);
