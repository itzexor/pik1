#include "test_session_harness.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <termios.h>

static int failures;

/* wire channel of the fixture's single PTY channel (CLI id 7) */
#define TEST_CH_ID   7u
#define TEST_WIRE_CH pik_mux_cli_to_wire(TEST_CH_ID)

typedef struct {
    session_fixture_t fx;
    char tmpdir[128];
    char pty_link[160];
} mux_fixture_t;

static bool fixture_init(mux_fixture_t *mx) {
    memset(mx, 0, sizeof(*mx));

    snprintf(mx->tmpdir, sizeof(mx->tmpdir), "/tmp/pik1-mux-test-%ld",
             (long)getpid());
    if (mkdir(mx->tmpdir, 0700) < 0 && errno != EEXIST) return false;
    snprintf(mx->pty_link, sizeof(mx->pty_link), "%s/ch7", mx->tmpdir);
    unlink(mx->pty_link);

    static serialmux_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.n_channels = 1;
    cfg.channels[0].type = CH_PTY;
    cfg.channels[0].ch_id = TEST_CH_ID;
    size_t link_len = strlen(mx->pty_link);
    if (link_len >= sizeof(cfg.channels[0].path)) return false;
    memcpy(cfg.channels[0].path, mx->pty_link, link_len + 1);

    session_fixture_cfg_t scfg = {
        .role = PIK_CONTROL_ROLE_PTY,
        .mux_cfg = &cfg,
    };
    return sfx_init(&mx->fx, &scfg);
}

static void fixture_cleanup(mux_fixture_t *mx) {
    sfx_cleanup(&mx->fx);
    unlink(mx->pty_link);
    if (mx->tmpdir[0]) rmdir(mx->tmpdir);
}

/* Time-travel dispatch: lets tests move the daemon's notion of "now". */
static int64_t g_now_offset;
static bool dispatch_offset(void *ptr, uint32_t events, int64_t now) {
    return sfx_dispatch(ptr, events, now + g_now_offset);
}

static bool send_mux(mux_fixture_t *mx, uint8_t type, uint8_t wire_ch,
                     const uint8_t *payload, size_t plen) {
    return sfx_send_peer_frame(&mx->fx, type, wire_ch, payload, plen);
}

static bool read_queued_frame(mux_fixture_t *mx, sfx_frame_t *f) {
    for (int i = 0; i < 8; i++) {
        int64_t now = pik_now_ms();
        if (!sfx_tick(now)) return false;
        if (!sfx_drain_link_tx(&mx->fx, now)) return false;
        if (sfx_take_tx_frame(&mx->fx, f->enc, sizeof(f->enc), &f->enc_len)) {
            uint8_t dec[8192];
            pik_frame_t frame;
            if (pik_frame_decode(f->enc, f->enc_len, sizeof(f->enc),
                                 dec, sizeof(dec), &frame) != PIK_FRAME_OK)
                return false;
            f->type = frame.header[0];
            f->ch = frame.header[1];
            f->session = pik_get_u32le(frame.header + 2);
            f->seq = (uint16_t)frame.header[6] | (uint16_t)frame.header[7] << 8;
            f->payload_len = frame.payload_len;
            if (f->payload_len)
                memcpy(f->payload, frame.payload, f->payload_len);
            if (f->type != PIK_FRAME_NAK)
                mx->fx.local_session = f->session;
            return true;
        }
    }
    return false;
}

static void test_ready_and_flush_manage_pty(void) {
    mux_fixture_t mx;
    CHECK(fixture_init(&mx));
    CHECK(sfx_handshake(&mx.fx));

    CHECK(send_mux(&mx, PIK_FRAME_MUX_READY, TEST_WIRE_CH, NULL, 0));
    CHECK(sfx_dispatch_one(&mx.fx, 1000));
    CHECK(access(mx.pty_link, F_OK) == 0);

    CHECK(send_mux(&mx, PIK_FRAME_MUX_FLUSH, TEST_WIRE_CH, NULL, 0));
    CHECK(sfx_settle(&mx.fx));
    CHECK(access(mx.pty_link, F_OK) != 0);
    fixture_cleanup(&mx);
}

static void test_created_pty_is_raw(void) {
    mux_fixture_t mx;
    CHECK(fixture_init(&mx));
    CHECK(sfx_handshake(&mx.fx));

    CHECK(send_mux(&mx, PIK_FRAME_MUX_READY, TEST_WIRE_CH, NULL, 0));
    CHECK(sfx_dispatch_one(&mx.fx, 1000));

    int pty_fd = open(mx.pty_link, O_RDWR | O_NONBLOCK | O_NOCTTY);
    CHECK(pty_fd >= 0);
    struct termios tio;
    CHECK(tcgetattr(pty_fd, &tio) == 0);
    CHECK((tio.c_lflag & (ICANON | ECHO | ISIG | IEXTEN)) == 0);
    CHECK((tio.c_iflag & (ICRNL | IXON | IXOFF)) == 0);
    CHECK((tio.c_oflag & OPOST) == 0);
    close(pty_fd);
    fixture_cleanup(&mx);
}

static void test_pty_survives_absent_reader(void) {
    mux_fixture_t mx;
    sfx_frame_t f;
    uint8_t stale[] = { 'o', 'l', 'd' };

    CHECK(fixture_init(&mx));
    CHECK(sfx_handshake(&mx.fx));

    CHECK(send_mux(&mx, PIK_FRAME_MUX_READY, TEST_WIRE_CH, NULL, 0));
    CHECK(sfx_dispatch_one(&mx.fx, 1000));
    CHECK(access(mx.pty_link, F_OK) == 0);

    CHECK(send_mux(&mx, PIK_FRAME_MUX_DATA, TEST_WIRE_CH, stale, sizeof(stale)));
    CHECK(sfx_settle(&mx.fx));
    for (int i = 0; i < 8; i++)
        (void)sfx_dispatch_one(&mx.fx, 20);
    CHECK(pik_session_up());
    CHECK(access(mx.pty_link, F_OK) == 0);

    int pty_fd = open(mx.pty_link, O_RDWR | O_NONBLOCK | O_NOCTTY);
    CHECK(pty_fd >= 0);
    uint8_t got[8];
    ssize_t n = read(pty_fd, got, sizeof(got));
    CHECK(n < 0 && errno == EAGAIN);
    CHECK(write(pty_fd, "hello", 5) == 5);
    CHECK(sfx_read_frame(&mx.fx, &f));
    CHECK(f.type == PIK_FRAME_MUX_DATA);
    CHECK(f.ch == TEST_WIRE_CH);
    CHECK(f.payload_len == 5 && memcmp(f.payload, "hello", 5) == 0);
    close(pty_fd);
    fixture_cleanup(&mx);
}

static void test_pty_edge_event_drains_input(void) {
    mux_fixture_t mx;
    sfx_frame_t f;
    uint8_t payload[PIK_MUX_MAX_PAYLOAD + 37u];
    uint8_t got[sizeof(payload)];
    size_t got_len = 0;
    unsigned frames = 0;

    CHECK(fixture_init(&mx));
    CHECK(sfx_handshake(&mx.fx));
    CHECK(send_mux(&mx, PIK_FRAME_MUX_READY, TEST_WIRE_CH, NULL, 0));
    CHECK(sfx_dispatch_one(&mx.fx, 1000));

    int pty_fd = open(mx.pty_link, O_WRONLY | O_NONBLOCK | O_NOCTTY);
    CHECK(pty_fd >= 0);
    for (size_t i = 0; i < sizeof(payload); i++)
        payload[i] = (uint8_t)i;
    CHECK(test_write_all(pty_fd, payload, sizeof(payload)));

    CHECK(test_epoll_dispatch_one(mx.fx.epfd, sfx_dispatch, 1000));
    while (read_queued_frame(&mx, &f)) {
        CHECK(f.type == PIK_FRAME_MUX_DATA);
        CHECK(f.ch == TEST_WIRE_CH);
        CHECK(got_len + f.payload_len <= sizeof(got));
        if (got_len + f.payload_len <= sizeof(got))
            memcpy(got + got_len, f.payload, f.payload_len);
        got_len += f.payload_len;
        frames++;
    }
    CHECK(frames > 1);
    CHECK(got_len == sizeof(payload));
    CHECK(memcmp(got, payload, sizeof(payload)) == 0);

    close(pty_fd);
    fixture_cleanup(&mx);
}

static void test_data_for_closed_pty_fails(void) {
    mux_fixture_t mx;
    uint8_t payload[] = { 1, 2, 3 };
    CHECK(fixture_init(&mx));
    CHECK(sfx_handshake(&mx.fx));
    CHECK(send_mux(&mx, PIK_FRAME_MUX_DATA, TEST_WIRE_CH, payload, sizeof(payload)));
    CHECK(!sfx_dispatch_one(&mx.fx, 1000));
    fixture_cleanup(&mx);
}

static void test_unknown_channel_fails(void) {
    mux_fixture_t mx;
    CHECK(fixture_init(&mx));
    CHECK(sfx_handshake(&mx.fx));
    /* valid wire range but not a configured channel */
    CHECK(send_mux(&mx, PIK_FRAME_MUX_READY, 3, NULL, 0));
    CHECK(!sfx_dispatch_one(&mx.fx, 1000));
    fixture_cleanup(&mx);
}

static void test_reserved_channel_fails(void) {
    mux_fixture_t mx;
    CHECK(fixture_init(&mx));
    CHECK(sfx_handshake(&mx.fx));
    /* mux-namespace type on a reserved channel: rejected by the router */
    CHECK(send_mux(&mx, PIK_FRAME_MUX_READY, 9, NULL, 0));
    CHECK(!sfx_dispatch_one(&mx.fx, 1000));
    fixture_cleanup(&mx);
}

static void test_unknown_type_fails(void) {
    mux_fixture_t mx;
    CHECK(fixture_init(&mx));
    CHECK(sfx_handshake(&mx.fx));
    CHECK(send_mux(&mx, PIK_FRAME_MUX_READY, TEST_WIRE_CH, NULL, 0));
    CHECK(sfx_dispatch_one(&mx.fx, 1000));
    CHECK(send_mux(&mx, 0x04, TEST_WIRE_CH, NULL, 0));
    CHECK(!sfx_dispatch_one(&mx.fx, 1000));
    fixture_cleanup(&mx);
}

static void test_stale_bringup_frames_discarded_then_sync(void) {
    mux_fixture_t mx;
    CHECK(fixture_init(&mx));

    /* stale mux traffic from the peer's previous session arrives before the
     * handshake: discarded (only HELLO can synchronize bring-up) */
    mx.fx.peer_seq = 5;
    CHECK(send_mux(&mx, PIK_FRAME_MUX_READY, TEST_WIRE_CH, NULL, 0));
    CHECK(send_mux(&mx, PIK_FRAME_MUX_READY, TEST_WIRE_CH, NULL, 0));
    CHECK(sfx_dispatch_one(&mx.fx, 1000));
    CHECK(access(mx.pty_link, F_OK) != 0);

    /* the peer restarts: its fresh session synchronizes normally */
    mx.fx.peer_session++;
    mx.fx.peer_seq = 0;
    CHECK(sfx_handshake(&mx.fx));
    CHECK(send_mux(&mx, PIK_FRAME_MUX_READY, TEST_WIRE_CH, NULL, 0));
    CHECK(sfx_dispatch_one(&mx.fx, 1000));
    CHECK(access(mx.pty_link, F_OK) == 0);
    fixture_cleanup(&mx);
}

static void test_stale_frames_past_grace_fail(void) {
    mux_fixture_t mx;
    CHECK(fixture_init(&mx));
    mx.fx.peer_seq = 5;
    CHECK(send_mux(&mx, PIK_FRAME_MUX_READY, TEST_WIRE_CH, NULL, 0));
    g_now_offset = 2001;
    CHECK(!test_epoll_dispatch_one(mx.fx.epfd, dispatch_offset, 1000));
    g_now_offset = 0;
    fixture_cleanup(&mx);
}

static void test_seq_gap_naks_and_heals(void) {
    mux_fixture_t mx;
    sfx_frame_t f;
    uint8_t payload_a[] = { 'a', 'b' };
    uint8_t payload_b[] = { 'h', 'i' };
    uint8_t got[8];

    CHECK(fixture_init(&mx));
    CHECK(sfx_handshake(&mx.fx));
    CHECK(send_mux(&mx, PIK_FRAME_MUX_READY, TEST_WIRE_CH, NULL, 0));
    CHECK(sfx_dispatch_one(&mx.fx, 1000));
    int pty_fd = open(mx.pty_link, O_RDONLY | O_NONBLOCK | O_NOCTTY);
    CHECK(pty_fd >= 0);
    struct termios tio;
    CHECK(tcgetattr(pty_fd, &tio) == 0);
    cfmakeraw(&tio);
    CHECK(tcsetattr(pty_fd, TCSANOW, &tio) == 0);

    /* drop a frame: peer skips a seq, link must survive and NAK */
    uint16_t lost_seq = mx.fx.peer_seq;
    mx.fx.peer_seq++;
    CHECK(send_mux(&mx, PIK_FRAME_MUX_DATA, TEST_WIRE_CH, payload_b, sizeof(payload_b)));
    CHECK(sfx_dispatch_one(&mx.fx, 1000));
    CHECK(sfx_read_frame(&mx.fx, &f));
    CHECK(f.type == PIK_FRAME_NAK);
    CHECK(f.payload_len == 2);
    CHECK(((uint16_t)f.payload[0] | (uint16_t)f.payload[1] << 8) == lost_seq);

    /* retransmit from the lost seq: stream heals, both payloads in order */
    mx.fx.peer_seq = lost_seq;
    CHECK(send_mux(&mx, PIK_FRAME_MUX_DATA, TEST_WIRE_CH, payload_a, sizeof(payload_a)));
    CHECK(send_mux(&mx, PIK_FRAME_MUX_DATA, TEST_WIRE_CH, payload_b, sizeof(payload_b)));
    CHECK(sfx_dispatch_one(&mx.fx, 1000));
    while (sfx_dispatch_one(&mx.fx, 100)) {}
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
    CHECK(send_mux(&mx, PIK_FRAME_MUX_FLUSH, TEST_WIRE_CH, NULL, 0));
    for (int i = 0; i < 8 && access(mx.pty_link, F_OK) == 0; i++)
        CHECK(sfx_dispatch_one(&mx.fx, 1000));
    CHECK(access(mx.pty_link, F_OK) != 0);
    close(pty_fd);
    fixture_cleanup(&mx);
}

static void test_seq_gap_budget_fails(void) {
    mux_fixture_t mx;
    CHECK(fixture_init(&mx));
    CHECK(sfx_handshake(&mx.fx));
    CHECK(send_mux(&mx, PIK_FRAME_MUX_READY, TEST_WIRE_CH, NULL, 0));
    CHECK(sfx_dispatch_one(&mx.fx, 1000));
    mx.fx.peer_seq++;
    CHECK(send_mux(&mx, PIK_FRAME_MUX_FLUSH, TEST_WIRE_CH, NULL, 0));
    CHECK(sfx_dispatch_one(&mx.fx, 1000));
    CHECK(sfx_tick(pik_now_ms()));
    CHECK(!sfx_tick(pik_now_ms() + 600));
    fixture_cleanup(&mx);
}

static void test_duplicate_frame_discarded(void) {
    mux_fixture_t mx;
    CHECK(fixture_init(&mx));
    CHECK(sfx_handshake(&mx.fx));
    uint16_t ready_seq = mx.fx.peer_seq;
    CHECK(send_mux(&mx, PIK_FRAME_MUX_READY, TEST_WIRE_CH, NULL, 0));
    CHECK(sfx_dispatch_one(&mx.fx, 1000));
    int pty_fd = open(mx.pty_link, O_RDONLY | O_NONBLOCK | O_NOCTTY);
    CHECK(pty_fd >= 0);

    mx.fx.peer_seq = ready_seq; /* duplicate of the frame just delivered */
    CHECK(send_mux(&mx, PIK_FRAME_MUX_READY, TEST_WIRE_CH, NULL, 0));
    CHECK(sfx_settle(&mx.fx));
    CHECK(access(mx.pty_link, F_OK) == 0);

    CHECK(send_mux(&mx, PIK_FRAME_MUX_FLUSH, TEST_WIRE_CH, NULL, 0));
    CHECK(sfx_settle(&mx.fx));
    CHECK(access(mx.pty_link, F_OK) != 0);
    close(pty_fd);
    fixture_cleanup(&mx);
}

static void test_nak_triggers_byte_identical_retransmit(void) {
    mux_fixture_t mx;
    sfx_frame_t f1, f2;
    CHECK(fixture_init(&mx));
    CHECK(sfx_handshake(&mx.fx));
    CHECK(send_mux(&mx, PIK_FRAME_MUX_READY, TEST_WIRE_CH, NULL, 0));
    CHECK(sfx_dispatch_one(&mx.fx, 1000));

    int pty_fd = open(mx.pty_link, O_WRONLY | O_NONBLOCK | O_NOCTTY);
    CHECK(pty_fd >= 0);
    CHECK(write(pty_fd, "hello", 5) == 5);
    CHECK(sfx_read_frame(&mx.fx, &f1));
    CHECK(f1.type == PIK_FRAME_MUX_DATA);
    CHECK(f1.ch == TEST_WIRE_CH);
    CHECK(f1.payload_len == 5 && memcmp(f1.payload, "hello", 5) == 0);

    /* peer claims it never saw that seq: resend must be byte-identical */
    CHECK(sfx_send_peer_nak(&mx.fx, f1.seq));
    CHECK(sfx_dispatch_one(&mx.fx, 1000));
    CHECK(sfx_read_frame(&mx.fx, &f2));
    CHECK(f2.enc_len == f1.enc_len);
    CHECK(memcmp(f2.enc, f1.enc, f1.enc_len) == 0);
    close(pty_fd);
    fixture_cleanup(&mx);
}

static void test_nak_beyond_window_fails(void) {
    mux_fixture_t mx;
    CHECK(fixture_init(&mx));
    CHECK(sfx_handshake(&mx.fx));
    CHECK(sfx_send_peer_nak(&mx.fx, 100)); /* nothing near that in history */
    CHECK(!sfx_dispatch_one(&mx.fx, 1000));
    fixture_cleanup(&mx);
}

static void test_session_change_fails(void) {
    mux_fixture_t mx;
    CHECK(fixture_init(&mx));
    CHECK(sfx_handshake(&mx.fx));
    CHECK(send_mux(&mx, PIK_FRAME_MUX_READY, TEST_WIRE_CH, NULL, 0));
    CHECK(sfx_dispatch_one(&mx.fx, 1000));
    mx.fx.peer_session++;
    CHECK(send_mux(&mx, PIK_FRAME_MUX_FLUSH, TEST_WIRE_CH, NULL, 0));
    CHECK(!sfx_dispatch_one(&mx.fx, 1000));
    fixture_cleanup(&mx);
}

int main(void) {
    test_ready_and_flush_manage_pty();
    test_created_pty_is_raw();
    test_pty_survives_absent_reader();
    test_pty_edge_event_drains_input();
    test_data_for_closed_pty_fails();
    test_unknown_channel_fails();
    test_reserved_channel_fails();
    test_unknown_type_fails();
    test_stale_bringup_frames_discarded_then_sync();
    test_stale_frames_past_grace_fail();
    test_seq_gap_naks_and_heals();
    test_seq_gap_budget_fails();
    test_duplicate_frame_discarded();
    test_nak_triggers_byte_identical_retransmit();
    test_nak_beyond_window_fails();
    test_session_change_fails();

    if (failures) {
        fprintf(stderr, "test_serialmux_protocol: %d failure(s)\n", failures);
        return 1;
    }
    puts("test_serialmux_protocol: ok");
    return 0;
}
