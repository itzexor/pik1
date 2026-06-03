#include "util.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>

static int failures;

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #expr); \
        failures++; \
    } \
} while (0)

static void test_parse_uint8(void) {
    uint8_t out = 99;
    CHECK(pik_parse_uint8("0", &out) && out == 0);
    CHECK(pik_parse_uint8("255", &out) && out == 255);
    CHECK(!pik_parse_uint8("256", &out));
    CHECK(!pik_parse_uint8("-1", &out));
    CHECK(!pik_parse_uint8("12x", &out));
    CHECK(!pik_parse_uint8("", &out));
}

static void test_parse_port(void) {
    int out = 0;
    CHECK(pik_parse_port("1", &out) && out == 1);
    CHECK(pik_parse_port("65535", &out) && out == 65535);
    CHECK(!pik_parse_port("0", &out));
    CHECK(!pik_parse_port("65536", &out));
    CHECK(!pik_parse_port("-1", &out));
    CHECK(!pik_parse_port("80/tcp", &out));
    CHECK(!pik_parse_port("", &out));
}

static void test_parse_positive_int(void) {
    int out = 0;
    CHECK(pik_parse_positive_int("1", &out) && out == 1);
    CHECK(pik_parse_positive_int("2147483647", &out) && out == INT_MAX);
    CHECK(!pik_parse_positive_int("0", &out));
    CHECK(!pik_parse_positive_int("-10", &out));
    CHECK(!pik_parse_positive_int("2147483648", &out));
    CHECK(!pik_parse_positive_int("123abc", &out));
    CHECK(!pik_parse_positive_int("", &out));
}

static void test_backoff(void) {
    int b = 1000;
    CHECK(pik_backoff_next(&b, 30000) == 1000);
    CHECK(b == 2000);
    CHECK(pik_backoff_next(&b, 30000) == 2000);
    CHECK(b == 4000);
    b = 20000;
    CHECK(pik_backoff_next(&b, 30000) == 20000);
    CHECK(b == 30000);
    CHECK(pik_backoff_next(&b, 30000) == 30000);
    CHECK(b == 30000);
}

static void test_le32(void) {
    uint8_t p[4];
    pik_put_u32le(p, 0x12345678u);
    CHECK(p[0] == 0x78);
    CHECK(p[1] == 0x56);
    CHECK(p[2] == 0x34);
    CHECK(p[3] == 0x12);
    CHECK(pik_get_u32le(p) == 0x12345678u);
}

int main(void) {
    test_parse_uint8();
    test_parse_port();
    test_parse_positive_int();
    test_backoff();
    test_le32();

    if (failures) {
        fprintf(stderr, "test_util: %d failure(s)\n", failures);
        return 1;
    }
    puts("test_util: ok");
    return 0;
}
