#include "serialmux.h"
#include "serialmux_proto.h"
#include "test_harness.h"

#include <fcntl.h>
#include <pty.h>
#include <stdbool.h>
#include <termios.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;

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

/* Time-travel dispatch: lets tests move the mux's notion of "now". */
static int64_t g_now_offset;
static bool dispatch_mux_offset(void *ptr, uint32_t events, int64_t now) {
    return serialmux_dispatch(ptr, events, now + g_now_offset);
}

typedef struct {
    uint8_t  type;
    uint8_t  ch_id;
    uint32_t session;
    uint16_t seq;
    uint8_t  payload[PIK_SERIALMUX_MAX_PAYLOAD];
    size_t   payload_len;
    uint8_t  enc[8192];
    size_t   enc_len;
} captured_frame_t;

/* Read one frame the mux wrote to the link, dispatching as needed to let it
 * drain its TX ring. */
static bool read_mux_frame(mux_fixture_t *fx, captured_frame_t *f) {
    for (int i = 0; i < 8; i++) {
        if (test_read_delimited_frame(fx->master, f->enc, sizeof(f->enc),
                                      &f->enc_len, 20)) {
            uint8_t dec[8192];
            pik_frame_t frame;
            if (pik_frame_decode(f->enc, f->enc_len, sizeof(f->enc),
                                 PIK_SERIALMUX_FRAME_HEADER_LEN,
                                 dec, sizeof(dec), &frame) != PIK_FRAME_OK)
                return false;
            f->type = frame.header[0];
            f->ch_id = frame.header[1];
            f->session = pik_get_u32le(frame.header + 2);
            f->seq = (uint16_t)frame.header[6] | (uint16_t)frame.header[7] << 8;
            f->payload_len = frame.payload_len;
            if (f->payload_len)
                memcpy(f->payload, frame.payload, f->payload_len);
            return true;
        }
        test_epoll_dispatch_one(fx->epfd, dispatch_mux, 100);
    }
    return false;
}

static bool send_peer_nak(mux_fixture_t *fx, uint16_t expected) {
    uint8_t header[PIK_SERIALMUX_FRAME_HEADER_LEN];
    uint8_t payload[2] = { (uint8_t)expected, (uint8_t)(expected >> 8) };
    uint8_t enc[128];
    size_t enc_len = 0;
    header[0] = PIK_SERIALMUX_FRAME_NAK;
    header[1] = 0;
    pik_put_u32le(header + 2, fx->peer_session);
    put_u16le(header + 6, fx->peer_seq); /* link-control: seq not consumed */
    if (!test_encode_frame(header, sizeof(header), payload, sizeof(payload),
                           enc, sizeof(enc), &enc_len))
        return false;
    return test_write_all(fx->master, enc, enc_len);
}

static bool send_peer_frame(mux_fixture_t *fx, uint8_t type, uint8_t ch_id,
                            const uint8_t *payload, size_t payload_len) {
    uint8_t header[PIK_SERIALMUX_FRAME_HEADER_LEN];
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
    uint8_t header[PIK_SERIALMUX_FRAME_HEADER_LEN];
    uint8_t enc[128];
    size_t enc_len = 0;
    header[0] = PIK_SERIALMUX_FRAME_READY;
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

    CHECK(send_peer_frame(&fx, PIK_SERIALMUX_FRAME_READY, 7, NULL, 0));
    CHECK(test_epoll_dispatch_one(fx.epfd, dispatch_mux, 1000));
    CHECK(access(fx.pty_link, F_OK) == 0);

    CHECK(send_peer_frame(&fx, PIK_SERIALMUX_FRAME_FLUSH, 7, NULL, 0));
    CHECK(test_epoll_dispatch_one(fx.epfd, dispatch_mux, 1000));
    CHECK(access(fx.pty_link, F_OK) != 0);
    fixture_cleanup(&fx);
}

static void test_data_for_closed_pty_fails(void) {
    mux_fixture_t fx;
    uint8_t payload[] = { 1, 2, 3 };
    CHECK(fixture_init(&fx));
    CHECK(send_peer_frame(&fx, PIK_SERIALMUX_FRAME_DATA, 7, payload, sizeof(payload)));
    CHECK(!test_epoll_dispatch_one(fx.epfd, dispatch_mux, 1000));
    fixture_cleanup(&fx);
}

static void test_unknown_channel_fails(void) {
    mux_fixture_t fx;
    CHECK(fixture_init(&fx));
    CHECK(send_peer_frame(&fx, PIK_SERIALMUX_FRAME_READY, 9, NULL, 0));
    CHECK(!test_epoll_dispatch_one(fx.epfd, dispatch_mux, 1000));
    fixture_cleanup(&fx);
}

static void test_unknown_type_fails(void) {
    mux_fixture_t fx;
    CHECK(fixture_init(&fx));
    CHECK(send_peer_frame(&fx, PIK_SERIALMUX_FRAME_READY, 7, NULL, 0));
    CHECK(test_epoll_dispatch_one(fx.epfd, dispatch_mux, 1000));
    CHECK(send_peer_frame(&fx, 0xf0, 7, NULL, 0));
    CHECK(!test_epoll_dispatch_one(fx.epfd, dispatch_mux, 1000));
    fixture_cleanup(&fx);
}

static void test_stale_bringup_frames_discarded_then_sync(void) {
    mux_fixture_t fx;
    CHECK(fixture_init(&fx));

    /* stale traffic from the peer's previous session: discarded, link lives */
    fx.peer_seq = 5;
    CHECK(send_peer_frame(&fx, PIK_SERIALMUX_FRAME_READY, 7, NULL, 0));
    CHECK(send_peer_frame(&fx, PIK_SERIALMUX_FRAME_READY, 7, NULL, 0));
    CHECK(test_epoll_dispatch_one(fx.epfd, dispatch_mux, 1000));
    CHECK(access(fx.pty_link, F_OK) != 0);

    /* the peer restarts: its fresh session synchronizes normally */
    fx.peer_session++;
    fx.peer_seq = 0;
    CHECK(send_peer_frame(&fx, PIK_SERIALMUX_FRAME_READY, 7, NULL, 0));
    CHECK(test_epoll_dispatch_one(fx.epfd, dispatch_mux, 1000));
    CHECK(access(fx.pty_link, F_OK) == 0);
    fixture_cleanup(&fx);
}

static void test_stale_frames_past_grace_fail(void) {
    mux_fixture_t fx;
    CHECK(fixture_init(&fx));
    fx.peer_seq = 5;
    CHECK(send_peer_frame(&fx, PIK_SERIALMUX_FRAME_READY, 7, NULL, 0));
    g_now_offset = 2001;
    CHECK(!test_epoll_dispatch_one(fx.epfd, dispatch_mux_offset, 1000));
    g_now_offset = 0;
    fixture_cleanup(&fx);
}

static void test_seq_gap_naks_and_heals(void) {
    mux_fixture_t fx;
    captured_frame_t f;
    uint8_t payload_a[] = { 'a', 'b' };
    uint8_t payload_b[] = { 'h', 'i' };
    uint8_t got[8];

    CHECK(fixture_init(&fx));
    CHECK(send_peer_frame(&fx, PIK_SERIALMUX_FRAME_READY, 7, NULL, 0));
    CHECK(test_epoll_dispatch_one(fx.epfd, dispatch_mux, 1000));
    int pty_fd = open(fx.pty_link, O_RDONLY | O_NONBLOCK | O_NOCTTY);
    CHECK(pty_fd >= 0);
    struct termios tio;
    CHECK(tcgetattr(pty_fd, &tio) == 0);
    cfmakeraw(&tio);
    CHECK(tcsetattr(pty_fd, TCSANOW, &tio) == 0);

    /* drop frame seq=1: peer sends seq=2, link must survive and NAK */
    fx.peer_seq = 2;
    CHECK(send_peer_frame(&fx, PIK_SERIALMUX_FRAME_DATA, 7, payload_b, sizeof(payload_b)));
    CHECK(test_epoll_dispatch_one(fx.epfd, dispatch_mux, 1000));
    CHECK(read_mux_frame(&fx, &f));
    CHECK(f.type == PIK_SERIALMUX_FRAME_NAK);
    CHECK(f.payload_len == 2);
    CHECK(((uint16_t)f.payload[0] | (uint16_t)f.payload[1] << 8) == 1);

    /* retransmit from seq=1: stream heals, both payloads delivered in order */
    fx.peer_seq = 1;
    CHECK(send_peer_frame(&fx, PIK_SERIALMUX_FRAME_DATA, 7, payload_a, sizeof(payload_a)));
    CHECK(send_peer_frame(&fx, PIK_SERIALMUX_FRAME_DATA, 7, payload_b, sizeof(payload_b)));
    CHECK(test_epoll_dispatch_one(fx.epfd, dispatch_mux, 1000));
    while (test_epoll_dispatch_one(fx.epfd, dispatch_mux, 100)) {}
    CHECK(test_wait_fd(pty_fd, false, 1000));
    ssize_t n = read(pty_fd, got, sizeof(got));
    while (n >= 0 && (size_t)n < 4) {
        if (!test_wait_fd(pty_fd, false, 1000)) break;
        ssize_t m = read(pty_fd, got + n, sizeof(got) - (size_t)n);
        if (m <= 0) break;
        n += m;
    }
    CHECK(n == 4);
    CHECK(memcmp(got, "abhi", 4) == 0);

    /* stream continues normally after the heal */
    CHECK(send_peer_frame(&fx, PIK_SERIALMUX_FRAME_FLUSH, 7, NULL, 0));
    CHECK(test_epoll_dispatch_one(fx.epfd, dispatch_mux, 1000));
    CHECK(access(fx.pty_link, F_OK) != 0);
    close(pty_fd);
    fixture_cleanup(&fx);
}

static void test_seq_gap_budget_fails(void) {
    mux_fixture_t fx;
    CHECK(fixture_init(&fx));
    CHECK(send_peer_frame(&fx, PIK_SERIALMUX_FRAME_READY, 7, NULL, 0));
    CHECK(test_epoll_dispatch_one(fx.epfd, dispatch_mux, 1000));
    fx.peer_seq = 2;
    CHECK(send_peer_frame(&fx, PIK_SERIALMUX_FRAME_FLUSH, 7, NULL, 0));
    CHECK(test_epoll_dispatch_one(fx.epfd, dispatch_mux, 1000));
    CHECK(serialmux_tick(pik_now_ms()));
    CHECK(!serialmux_tick(pik_now_ms() + 600));
    fixture_cleanup(&fx);
}

static void test_duplicate_frame_discarded(void) {
    mux_fixture_t fx;
    CHECK(fixture_init(&fx));
    CHECK(send_peer_frame(&fx, PIK_SERIALMUX_FRAME_READY, 7, NULL, 0));
    CHECK(test_epoll_dispatch_one(fx.epfd, dispatch_mux, 1000));

    fx.peer_seq = 0; /* duplicate of the frame just delivered */
    CHECK(send_peer_frame(&fx, PIK_SERIALMUX_FRAME_READY, 7, NULL, 0));
    CHECK(test_epoll_dispatch_one(fx.epfd, dispatch_mux, 1000));
    CHECK(access(fx.pty_link, F_OK) == 0);

    CHECK(send_peer_frame(&fx, PIK_SERIALMUX_FRAME_FLUSH, 7, NULL, 0));
    CHECK(test_epoll_dispatch_one(fx.epfd, dispatch_mux, 1000));
    CHECK(access(fx.pty_link, F_OK) != 0);
    fixture_cleanup(&fx);
}

static void test_nak_triggers_byte_identical_retransmit(void) {
    mux_fixture_t fx;
    captured_frame_t f1, f2;
    CHECK(fixture_init(&fx));
    CHECK(send_peer_frame(&fx, PIK_SERIALMUX_FRAME_READY, 7, NULL, 0));
    CHECK(test_epoll_dispatch_one(fx.epfd, dispatch_mux, 1000));

    int pty_fd = open(fx.pty_link, O_WRONLY | O_NONBLOCK | O_NOCTTY);
    CHECK(pty_fd >= 0);
    CHECK(write(pty_fd, "hello", 5) == 5);
    CHECK(read_mux_frame(&fx, &f1));
    CHECK(f1.type == PIK_SERIALMUX_FRAME_DATA);
    CHECK(f1.seq == 0);
    CHECK(f1.payload_len == 5 && memcmp(f1.payload, "hello", 5) == 0);

    /* peer claims it never saw seq=0: resent frame must be byte-identical */
    CHECK(send_peer_nak(&fx, 0));
    CHECK(test_epoll_dispatch_one(fx.epfd, dispatch_mux, 1000));
    CHECK(read_mux_frame(&fx, &f2));
    CHECK(f2.enc_len == f1.enc_len);
    CHECK(memcmp(f2.enc, f1.enc, f1.enc_len) == 0);
    close(pty_fd);
    fixture_cleanup(&fx);
}

static void test_nak_beyond_window_fails(void) {
    mux_fixture_t fx;
    CHECK(fixture_init(&fx));
    CHECK(send_peer_frame(&fx, PIK_SERIALMUX_FRAME_READY, 7, NULL, 0));
    CHECK(test_epoll_dispatch_one(fx.epfd, dispatch_mux, 1000));
    CHECK(send_peer_nak(&fx, 100)); /* nothing near that in history */
    CHECK(!test_epoll_dispatch_one(fx.epfd, dispatch_mux, 1000));
    fixture_cleanup(&fx);
}

static void test_session_change_fails(void) {
    mux_fixture_t fx;
    CHECK(fixture_init(&fx));
    CHECK(send_peer_frame(&fx, PIK_SERIALMUX_FRAME_READY, 7, NULL, 0));
    CHECK(test_epoll_dispatch_one(fx.epfd, dispatch_mux, 1000));
    fx.peer_session++;
    CHECK(send_peer_frame(&fx, PIK_SERIALMUX_FRAME_FLUSH, 7, NULL, 0));
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
    test_stale_bringup_frames_discarded_then_sync();
    test_stale_frames_past_grace_fail();
    test_seq_gap_naks_and_heals();
    test_seq_gap_budget_fails();
    test_duplicate_frame_discarded();
    test_nak_triggers_byte_identical_retransmit();
    test_nak_beyond_window_fails();
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
