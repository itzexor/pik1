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

static void roundtrip(const uint8_t *dec, size_t dec_len) {
    uint8_t enc[COBS_ENCODE_MAX(512)];
    uint8_t out[512];
    size_t enc_len = 0, out_len = 0;

    CHECK(cobs_encode(dec, dec_len, enc, sizeof(enc), &enc_len) == COBS_RET_SUCCESS);
    CHECK(enc_len >= 2);
    CHECK(enc[enc_len - 1] == COBS_FRAME_DELIMITER);
    CHECK(cobs_decode(enc, enc_len, out, sizeof(out), &out_len) == COBS_RET_SUCCESS);
    CHECK(out_len == dec_len);
    CHECK(memcmp(out, dec, dec_len) == 0);
}

static void test_roundtrips(void) {
    uint8_t empty[] = { 0 };
    uint8_t zeros[] = { 0, 0, 0, 0, 0 };
    uint8_t mixed[] = { 0x11, 0, 0x22, 0x33, 0, 0x44 };
    uint8_t long_run[255];
    uint8_t with_zero[300];

    for (size_t i = 0; i < sizeof(long_run); i++)
        long_run[i] = (uint8_t)(i + 1);
    for (size_t i = 0; i < sizeof(with_zero); i++)
        with_zero[i] = (i % 57 == 0) ? 0 : (uint8_t)(i + 3);

    roundtrip(empty, 0);
    roundtrip(zeros, sizeof(zeros));
    roundtrip(mixed, sizeof(mixed));
    roundtrip(long_run, 254);
    roundtrip(long_run, 255);
    roundtrip(with_zero, sizeof(with_zero));
}

static void test_encode_errors(void) {
    uint8_t dec[] = { 1, 2, 3 };
    uint8_t enc[8];
    size_t enc_len = 0;

    CHECK(cobs_encode(NULL, sizeof(dec), enc, sizeof(enc), &enc_len) == COBS_RET_ERR_BAD_ARG);
    CHECK(cobs_encode(dec, sizeof(dec), NULL, sizeof(enc), &enc_len) == COBS_RET_ERR_BAD_ARG);
    CHECK(cobs_encode(dec, sizeof(dec), enc, sizeof(enc), NULL) == COBS_RET_ERR_BAD_ARG);
    CHECK(cobs_encode(dec, sizeof(dec), enc, 1, &enc_len) == COBS_RET_ERR_BAD_ARG);
    CHECK(cobs_encode(dec, sizeof(dec), enc, 2, &enc_len) == COBS_RET_ERR_EXHAUSTED);
}

static void test_decode_errors(void) {
    uint8_t out[8];
    size_t out_len = 0;
    uint8_t starts_zero[] = { 0, 1, 0 };
    uint8_t no_delim[] = { 1, 2, 3 };
    uint8_t bad_code[] = { 5, 1, 0 };
    uint8_t good[] = { 4, 1, 2, 3, 0 };

    CHECK(cobs_decode(NULL, sizeof(good), out, sizeof(out), &out_len) == COBS_RET_ERR_BAD_ARG);
    CHECK(cobs_decode(good, sizeof(good), NULL, sizeof(out), &out_len) == COBS_RET_ERR_BAD_ARG);
    CHECK(cobs_decode(good, sizeof(good), out, sizeof(out), NULL) == COBS_RET_ERR_BAD_ARG);
    CHECK(cobs_decode(starts_zero, sizeof(starts_zero), out, sizeof(out), &out_len) == COBS_RET_ERR_BAD_PAYLOAD);
    CHECK(cobs_decode(no_delim, sizeof(no_delim), out, sizeof(out), &out_len) == COBS_RET_ERR_EXHAUSTED);
    CHECK(cobs_decode(bad_code, sizeof(bad_code), out, sizeof(out), &out_len) == COBS_RET_ERR_BAD_PAYLOAD);
    CHECK(cobs_decode(good, sizeof(good), out, 2, &out_len) == COBS_RET_ERR_EXHAUSTED);
}

static void test_tinyframe(void) {
    uint8_t buf[] = {
        COBS_TINYFRAME_SENTINEL_VALUE,
        0x11, 0, 0x22,
        COBS_TINYFRAME_SENTINEL_VALUE
    };
    uint8_t bad[] = { 0, 1, COBS_TINYFRAME_SENTINEL_VALUE };

    CHECK(cobs_encode_tinyframe(NULL, 4) == COBS_RET_ERR_BAD_ARG);
    CHECK(cobs_encode_tinyframe(buf, 1) == COBS_RET_ERR_BAD_ARG);
    CHECK(cobs_encode_tinyframe(bad, sizeof(bad)) == COBS_RET_ERR_BAD_PAYLOAD);
    CHECK(cobs_encode_tinyframe(buf, sizeof(buf)) == COBS_RET_SUCCESS);
    CHECK(buf[sizeof(buf) - 1] == COBS_FRAME_DELIMITER);
    CHECK(cobs_decode_tinyframe(buf, sizeof(buf)) == COBS_RET_SUCCESS);
    CHECK(buf[0] == COBS_TINYFRAME_SENTINEL_VALUE);
    CHECK(buf[sizeof(buf) - 1] == COBS_TINYFRAME_SENTINEL_VALUE);
    CHECK(buf[1] == 0x11);
    CHECK(buf[2] == 0);
    CHECK(buf[3] == 0x22);
}

static void test_incremental_encode_matches_oneshot(void) {
    uint8_t dec[300];
    uint8_t one[COBS_ENCODE_MAX(sizeof(dec))];
    uint8_t inc[COBS_ENCODE_MAX(sizeof(dec))];
    uint8_t work[255];
    size_t one_len = 0, inc_len = 0;
    cobs_enc_ctx_t ctx;

    for (size_t i = 0; i < sizeof(dec); i++)
        dec[i] = (i % 61 == 0) ? 0 : (uint8_t)(i + 1);

    CHECK(cobs_encode(dec, sizeof(dec), one, sizeof(one), &one_len) == COBS_RET_SUCCESS);
    CHECK(cobs_encode_inc_begin(&ctx, work, sizeof(work)) == COBS_RET_SUCCESS);

    size_t pos = 0;
    while (pos < sizeof(dec)) {
        size_t chunk = sizeof(dec) - pos;
        if (chunk > 37) chunk = 37;
        cobs_encode_inc_args_t args = {
            .dec_src = dec + pos,
            .enc_dst = inc + inc_len,
            .dec_src_max = chunk,
            .enc_dst_max = sizeof(inc) - inc_len,
        };
        size_t consumed = 0, written = 0;
        CHECK(cobs_encode_inc(&ctx, &args, &consumed, &written) == COBS_RET_SUCCESS);
        CHECK(consumed <= chunk);
        CHECK(written <= sizeof(inc) - inc_len);
        pos += consumed;
        inc_len += written;
        if (consumed == 0 && written == 0)
            break;
    }

    bool finished = false;
    while (!finished) {
        size_t written = 0;
        CHECK(cobs_encode_inc_end(&ctx, inc + inc_len, sizeof(inc) - inc_len,
                                  &written, &finished) == COBS_RET_SUCCESS);
        inc_len += written;
    }

    CHECK(inc_len == one_len);
    CHECK(memcmp(inc, one, one_len) == 0);
}

int main(void) {
    test_roundtrips();
    test_encode_errors();
    test_decode_errors();
    test_tinyframe();
    test_incremental_encode_matches_oneshot();

    if (failures) {
        fprintf(stderr, "test_cobs: %d failure(s)\n", failures);
        return 1;
    }
    puts("test_cobs: ok");
    return 0;
}
