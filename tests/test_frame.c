#include "frame.h"
#include "nanocobs/cobs.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #expr); \
        failures++; \
    } \
} while (0)

typedef struct {
    int calls;
    size_t lens[8];
    unsigned first[8];
    bool fail_after_first;
} rx_ctx_t;

static bool rx_cb(void *vctx, const uint8_t *enc, size_t enc_len) {
    rx_ctx_t *ctx = vctx;
    if (ctx->calls < 8) {
        ctx->lens[ctx->calls] = enc_len;
        ctx->first[ctx->calls] = enc_len ? enc[0] : 0;
    }
    ctx->calls++;
    return !(ctx->fail_after_first && ctx->calls >= 1);
}

static void encode_sample(uint8_t *enc, size_t enc_cap, size_t *enc_len) {
    uint8_t header[PIK_WIRE_HEADER_LEN] = { 0x21, 0x03 };
    uint8_t payload[] = { 0x00, 0x11, 0x22, 0x00, 0x33 };
    uint8_t dec[64];
    CHECK(pik_frame_encode(header, payload, sizeof(payload),
                           dec, sizeof(dec), enc, enc_cap, enc_len) == PIK_FRAME_OK);
}

static void test_roundtrip(void) {
    uint8_t header[PIK_WIRE_HEADER_LEN] = { 0x21, 0x03 };
    uint8_t payload[] = { 0x00, 0x11, 0x22, 0x00, 0x33 };
    uint8_t dec[64], enc[COBS_ENCODE_MAX(sizeof(header) + sizeof(payload) + 4) + 1];
    uint8_t out[64];
    size_t enc_len = 0;
    pik_frame_t frame;

    CHECK(pik_frame_encode(header, payload, sizeof(payload),
                           dec, sizeof(dec), enc, sizeof(enc), &enc_len) == PIK_FRAME_OK);
    CHECK(enc_len > 0);
    CHECK(enc[enc_len - 1] == COBS_FRAME_DELIMITER);
    CHECK(pik_frame_decode(enc, enc_len, sizeof(enc),
                           out, sizeof(out), &frame) == PIK_FRAME_OK);
    CHECK(frame.payload_len == sizeof(payload));
    CHECK(memcmp(frame.header, header, sizeof(header)) == 0);
    CHECK(memcmp(frame.payload, payload, sizeof(payload)) == 0);
}

static void test_empty_payload(void) {
    uint8_t header[PIK_WIRE_HEADER_LEN] = { 0x05 };
    uint8_t dec[16], enc[32], out[16];
    size_t enc_len = 0;
    pik_frame_t frame;

    CHECK(pik_frame_encode(header, NULL, 0,
                           dec, sizeof(dec), enc, sizeof(enc), &enc_len) == PIK_FRAME_OK);
    CHECK(pik_frame_decode(enc, enc_len, sizeof(enc),
                           out, sizeof(out), &frame) == PIK_FRAME_OK);
    CHECK(frame.header[0] == 0x05);
    CHECK(frame.payload_len == 0);
}

static void test_encode_oversized(void) {
    uint8_t header[PIK_WIRE_HEADER_LEN] = { 1, 2 };
    uint8_t payload[] = { 3, 4 };
    uint8_t dec[PIK_WIRE_HEADER_LEN + sizeof(payload) + 3], enc[32];
    size_t enc_len = 0;

    CHECK(pik_frame_encode(header, payload, sizeof(payload),
                           dec, sizeof(dec), enc, sizeof(enc), &enc_len) == PIK_FRAME_OVERSIZED);
}

static void test_decode_oversized(void) {
    uint8_t enc[64];
    size_t enc_len = 0;
    uint8_t out[64];
    pik_frame_t frame;
    encode_sample(enc, sizeof(enc), &enc_len);

    CHECK(pik_frame_decode(enc, enc_len, enc_len - 1,
                           out, sizeof(out), &frame) == PIK_FRAME_OVERSIZED);
}

static void test_decode_short(void) {
    uint8_t enc[16], out[16];
    uint8_t tiny[] = { 0x01, COBS_FRAME_DELIMITER };
    size_t dec_len = 0;
    pik_frame_t frame;

    CHECK(cobs_encode(tiny, 1, enc, sizeof(enc), &dec_len) == COBS_RET_SUCCESS);
    CHECK(pik_frame_decode(enc, dec_len, sizeof(enc),
                           out, sizeof(out), &frame) == PIK_FRAME_SHORT);
}

static void test_crc_mismatch(void) {
    uint8_t enc[64], out[64];
    size_t enc_len = 0;
    pik_frame_t frame;
    encode_sample(enc, sizeof(enc), &enc_len);

    enc[enc_len > 2 ? 1 : 0] ^= 0x01;
    CHECK(pik_frame_decode(enc, enc_len, sizeof(enc),
                           out, sizeof(out), &frame) == PIK_FRAME_CRC_MISMATCH);
}

static void test_cobs_bad_payload(void) {
    uint8_t enc[] = { 0x05, 0x01, COBS_FRAME_DELIMITER };
    uint8_t out[16];
    pik_frame_t frame;

    CHECK(pik_frame_decode(enc, sizeof(enc), sizeof(enc),
                           out, sizeof(out), &frame) == PIK_FRAME_COBS_BAD_PAYLOAD);
}

static void test_rx_consume_multiple_frames(void) {
    uint8_t frame1[64], frame2[64], rxbuf[128];
    size_t len1 = 0, len2 = 0, rx_len = 0;
    rx_ctx_t ctx = { 0 };

    encode_sample(frame1, sizeof(frame1), &len1);
    encode_sample(frame2, sizeof(frame2), &len2);
    memcpy(rxbuf, frame1, len1);
    memcpy(rxbuf + len1, frame2, len2);
    rx_len = len1 + len2;

    CHECK(pik_frame_rx_consume(rxbuf, &rx_len, sizeof(rxbuf), rx_cb, &ctx) == PIK_FRAME_OK);
    CHECK(ctx.calls == 2);
    CHECK(ctx.lens[0] == len1);
    CHECK(ctx.lens[1] == len2);
    CHECK(rx_len == 0);
}

static void test_rx_consume_partial_frame(void) {
    uint8_t frame[64], rxbuf[128];
    size_t enc_len = 0, rx_len = 0;
    rx_ctx_t ctx = { 0 };

    encode_sample(frame, sizeof(frame), &enc_len);
    memcpy(rxbuf, frame, enc_len - 1);
    rx_len = enc_len - 1;
    CHECK(pik_frame_rx_consume(rxbuf, &rx_len, sizeof(rxbuf), rx_cb, &ctx) == PIK_FRAME_OK);
    CHECK(ctx.calls == 0);
    CHECK(rx_len == enc_len - 1);

    rxbuf[rx_len++] = frame[enc_len - 1];
    CHECK(pik_frame_rx_consume(rxbuf, &rx_len, sizeof(rxbuf), rx_cb, &ctx) == PIK_FRAME_OK);
    CHECK(ctx.calls == 1);
    CHECK(rx_len == 0);
}

static void test_rx_consume_empty_delimiters(void) {
    uint8_t rxbuf[] = { COBS_FRAME_DELIMITER, COBS_FRAME_DELIMITER };
    size_t rx_len = sizeof(rxbuf);
    rx_ctx_t ctx = { 0 };

    CHECK(pik_frame_rx_consume(rxbuf, &rx_len, sizeof(rxbuf), rx_cb, &ctx) == PIK_FRAME_OK);
    CHECK(ctx.calls == 0);
    CHECK(rx_len == 0);
}

static void test_rx_consume_empty_delimiters_before_frame(void) {
    uint8_t frame[64], rxbuf[96];
    size_t enc_len = 0, rx_len = 0;
    rx_ctx_t ctx = { 0 };

    encode_sample(frame, sizeof(frame), &enc_len);
    rxbuf[rx_len++] = COBS_FRAME_DELIMITER;
    rxbuf[rx_len++] = COBS_FRAME_DELIMITER;
    memcpy(rxbuf + rx_len, frame, enc_len);
    rx_len += enc_len;

    CHECK(pik_frame_rx_consume(rxbuf, &rx_len, sizeof(rxbuf), rx_cb, &ctx) == PIK_FRAME_OK);
    CHECK(ctx.calls == 1);
    CHECK(ctx.lens[0] == enc_len);
    CHECK(rx_len == 0);
}

static void test_rx_consume_callback_failed(void) {
    uint8_t frame[64], rxbuf[64];
    size_t enc_len = 0, rx_len = 0;
    rx_ctx_t ctx = { .fail_after_first = true };

    encode_sample(frame, sizeof(frame), &enc_len);
    memcpy(rxbuf, frame, enc_len);
    rx_len = enc_len;

    CHECK(pik_frame_rx_consume(rxbuf, &rx_len, sizeof(rxbuf), rx_cb, &ctx) ==
          PIK_FRAME_CALLBACK_FAILED);
    CHECK(ctx.calls == 1);
    CHECK(rx_len == enc_len);
}

static void test_rx_consume_overflow(void) {
    uint8_t rxbuf[] = { 1, 2, 3, 4 };
    size_t rx_len = sizeof(rxbuf);
    rx_ctx_t ctx = { 0 };

    CHECK(pik_frame_rx_consume(rxbuf, &rx_len, sizeof(rxbuf), rx_cb, &ctx) ==
          PIK_FRAME_RX_OVERFLOW);
    CHECK(ctx.calls == 0);
}

static void test_status_text(void) {
    CHECK(strcmp(pik_frame_status_text(PIK_FRAME_OK), "ok") == 0);
    CHECK(strcmp(pik_frame_status_text(PIK_FRAME_CRC_MISMATCH), "CRC mismatch") == 0);
    CHECK(strcmp(pik_frame_status_text((pik_frame_status_t)255),
                 "unknown frame status") == 0);
}

static void test_max_size_roundtrip(void) {
    uint8_t header[8];
    uint8_t payload[4096];
    uint8_t dec[sizeof(header) + sizeof(payload) + 4];
    uint8_t enc[COBS_ENCODE_MAX(sizeof(dec))];
    uint8_t out[sizeof(dec)];
    size_t enc_len = 0;
    pik_frame_t frame;

    for (size_t i = 0; i < sizeof(header); i++)
        header[i] = (uint8_t)(0x80u + i);
    for (size_t i = 0; i < sizeof(payload); i++)
        payload[i] = (uint8_t)i;

    CHECK(pik_frame_encode(header, payload, sizeof(payload),
                           dec, sizeof(dec), enc, sizeof(enc), &enc_len) == PIK_FRAME_OK);
    CHECK(pik_frame_decode(enc, enc_len, sizeof(enc),
                           out, sizeof(out), &frame) == PIK_FRAME_OK);
    CHECK(frame.payload_len == sizeof(payload));
    CHECK(memcmp(frame.header, header, sizeof(header)) == 0);
    CHECK(memcmp(frame.payload, payload, sizeof(payload)) == 0);
}

int main(void) {
    test_roundtrip();
    test_empty_payload();
    test_encode_oversized();
    test_decode_oversized();
    test_decode_short();
    test_crc_mismatch();
    test_cobs_bad_payload();
    test_rx_consume_multiple_frames();
    test_rx_consume_partial_frame();
    test_rx_consume_empty_delimiters();
    test_rx_consume_empty_delimiters_before_frame();
    test_rx_consume_callback_failed();
    test_rx_consume_overflow();
    test_status_text();
    test_max_size_roundtrip();

    if (failures) {
        fprintf(stderr, "test_frame: %d failure(s)\n", failures);
        return 1;
    }
    puts("test_frame: ok");
    return 0;
}
