#include "serialmux.h"
#include "test_harness.h"

#include <pty.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;

#define F_DATA   0x01u
#define F_FLUSH  0x02u
#define F_READY  0x03u

#define HEADER_LEN 8u

typedef struct {
    int epfd;
    int master;
    int slave_keepalive;
    char slave_name[128];
    char tmpdir[128];
    char pty_link[160];
    uint32_t peer_session;
    uint16_t peer_seq;
} mux_fixture_t;

static void put_u16le(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static bool fixture_init(mux_fixture_t *fx) {
    memset(fx, 0, sizeof(*fx));
    fx->epfd = epoll_create1(EPOLL_CLOEXEC);
    fx->master = -1;
    fx->slave_keepalive = -1;
    fx->peer_session = 0xbabe0001u;
    fx->peer_seq = 0;
    if (fx->epfd < 0) return false;
    if (openpty(&fx->master, &fx->slave_keepalive, fx->slave_name, NULL, NULL) < 0)
        return false;
    test_set_nonblock(fx->master);

    snprintf(fx->tmpdir, sizeof(fx->tmpdir), "/tmp/pik1-mux-test-%ld-%d",
             (long)getpid(), fx->epfd);
    if (mkdir(fx->tmpdir, 0700) < 0) return false;
    snprintf(fx->pty_link, sizeof(fx->pty_link), "%s/ch7", fx->tmpdir);

    serialmux_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.n_channels = 1;
    cfg.channels[0].type = CH_PTY;
    cfg.channels[0].ch_id = 7;
    size_t link_len = strlen(fx->pty_link);
    if (link_len >= sizeof(cfg.channels[0].path)) return false;
    memcpy(cfg.channels[0].path, fx->pty_link, link_len + 1);
    serialmux_init(&cfg, fx->epfd);
    return serialmux_start(fx->slave_name, pik_now_ms());
}

static void fixture_cleanup(mux_fixture_t *fx) {
    serialmux_cleanup();
    unlink(fx->pty_link);
    if (fx->tmpdir[0]) rmdir(fx->tmpdir);
    if (fx->master >= 0) close(fx->master);
    if (fx->slave_keepalive >= 0) close(fx->slave_keepalive);
    if (fx->epfd >= 0) close(fx->epfd);
}

static bool dispatch_mux(void *ptr, uint32_t events, int64_t now) {
    return serialmux_dispatch(ptr, events, now);
}

static bool send_peer_frame(mux_fixture_t *fx, uint8_t type, uint8_t ch_id,
                            const uint8_t *payload, size_t payload_len) {
    uint8_t header[HEADER_LEN];
    uint8_t enc[8192];
    size_t enc_len = 0;
    header[0] = type;
    header[1] = ch_id;
    pik_put_u32le(header + 2, fx->peer_session);
    put_u16le(header + 6, fx->peer_seq++);
    if (!test_encode_frame(header, sizeof(header), payload, payload_len,
                           enc, sizeof(enc), &enc_len))
        return false;
    return test_write_all(fx->master, enc, enc_len);
}

static bool send_corrupt_frame(mux_fixture_t *fx) {
    uint8_t header[HEADER_LEN];
    uint8_t enc[128];
    size_t enc_len = 0;
    header[0] = F_READY;
    header[1] = 7;
    pik_put_u32le(header + 2, fx->peer_session);
    put_u16le(header + 6, fx->peer_seq++);
    if (!test_encode_frame(header, sizeof(header), NULL, 0,
                           enc, sizeof(enc), &enc_len))
        return false;
    enc[1] ^= 0x20;
    return test_write_all(fx->master, enc, enc_len);
}

static void test_ready_and_flush_manage_pty(void) {
    mux_fixture_t fx;
    CHECK(fixture_init(&fx));

    CHECK(send_peer_frame(&fx, F_READY, 7, NULL, 0));
    CHECK(test_epoll_dispatch_one(fx.epfd, dispatch_mux, 1000));
    CHECK(access(fx.pty_link, F_OK) == 0);

    CHECK(send_peer_frame(&fx, F_FLUSH, 7, NULL, 0));
    CHECK(test_epoll_dispatch_one(fx.epfd, dispatch_mux, 1000));
    CHECK(access(fx.pty_link, F_OK) != 0);
    fixture_cleanup(&fx);
}

static void test_data_for_closed_pty_fails(void) {
    mux_fixture_t fx;
    uint8_t payload[] = { 1, 2, 3 };
    CHECK(fixture_init(&fx));
    CHECK(send_peer_frame(&fx, F_DATA, 7, payload, sizeof(payload)));
    CHECK(!test_epoll_dispatch_one(fx.epfd, dispatch_mux, 1000));
    fixture_cleanup(&fx);
}

static void test_unknown_channel_fails(void) {
    mux_fixture_t fx;
    CHECK(fixture_init(&fx));
    CHECK(send_peer_frame(&fx, F_READY, 9, NULL, 0));
    CHECK(!test_epoll_dispatch_one(fx.epfd, dispatch_mux, 1000));
    fixture_cleanup(&fx);
}

static void test_unknown_type_fails(void) {
    mux_fixture_t fx;
    CHECK(fixture_init(&fx));
    CHECK(send_peer_frame(&fx, F_READY, 7, NULL, 0));
    CHECK(test_epoll_dispatch_one(fx.epfd, dispatch_mux, 1000));
    CHECK(send_peer_frame(&fx, 0xf0, 7, NULL, 0));
    CHECK(!test_epoll_dispatch_one(fx.epfd, dispatch_mux, 1000));
    fixture_cleanup(&fx);
}

static void test_first_seq_must_be_zero(void) {
    mux_fixture_t fx;
    CHECK(fixture_init(&fx));
    fx.peer_seq = 1;
    CHECK(send_peer_frame(&fx, F_READY, 7, NULL, 0));
    CHECK(!test_epoll_dispatch_one(fx.epfd, dispatch_mux, 1000));
    fixture_cleanup(&fx);
}

static void test_sequence_gap_fails(void) {
    mux_fixture_t fx;
    CHECK(fixture_init(&fx));
    CHECK(send_peer_frame(&fx, F_READY, 7, NULL, 0));
    CHECK(test_epoll_dispatch_one(fx.epfd, dispatch_mux, 1000));
    fx.peer_seq++;
    CHECK(send_peer_frame(&fx, F_FLUSH, 7, NULL, 0));
    CHECK(!test_epoll_dispatch_one(fx.epfd, dispatch_mux, 1000));
    fixture_cleanup(&fx);
}

static void test_session_change_fails(void) {
    mux_fixture_t fx;
    CHECK(fixture_init(&fx));
    CHECK(send_peer_frame(&fx, F_READY, 7, NULL, 0));
    CHECK(test_epoll_dispatch_one(fx.epfd, dispatch_mux, 1000));
    fx.peer_session++;
    CHECK(send_peer_frame(&fx, F_FLUSH, 7, NULL, 0));
    CHECK(!test_epoll_dispatch_one(fx.epfd, dispatch_mux, 1000));
    fixture_cleanup(&fx);
}

static void test_bad_crc_fails(void) {
    mux_fixture_t fx;
    CHECK(fixture_init(&fx));
    CHECK(send_corrupt_frame(&fx));
    CHECK(!test_epoll_dispatch_one(fx.epfd, dispatch_mux, 1000));
    fixture_cleanup(&fx);
}

static void test_link_hup_fails(void) {
    mux_fixture_t fx;
    CHECK(fixture_init(&fx));
    close(fx.master);
    fx.master = -1;
    CHECK(!test_epoll_dispatch_one(fx.epfd, dispatch_mux, 1000));
    fixture_cleanup(&fx);
}

int main(void) {
    test_ready_and_flush_manage_pty();
    test_data_for_closed_pty_fails();
    test_unknown_channel_fails();
    test_unknown_type_fails();
    test_first_seq_must_be_zero();
    test_sequence_gap_fails();
    test_session_change_fails();
    test_bad_crc_fails();
    test_link_hup_fails();

    if (failures) {
        fprintf(stderr, "test_serialmux_protocol: %d failure(s)\n", failures);
        return 1;
    }
    puts("test_serialmux_protocol: ok");
    return 0;
}
