#include "control.h"
#include "control_proto.h"
#include "test_harness.h"
#include "version.h"

#include <pty.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>

static int failures;

typedef struct {
    int epfd;
    int master;
    int slave_keepalive;
    char slave_name[128];
    uint32_t peer_session;
    uint32_t local_session;   /* the daemon's TX session, learned at handshake */
    uint16_t peer_seq;
} ctrl_fixture_t;

static int command_calls;
static pik_control_action_t last_action;
static uint32_t last_request;

static void command_cb(pik_control_action_t action, uint32_t request_id, void *ctx) {
    (void)ctx;
    command_calls++;
    last_action = action;
    last_request = request_id;
}

static void put_u16le(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static bool write_stale_config(ctrl_fixture_t *fx) {
    uint8_t header[PIK_CONTROL_FRAME_HEADER_LEN];
    uint8_t payload[] = { PIK_CONTROL_TCP_NONE, 0 };
    uint8_t enc[128];
    size_t enc_len = 0;
    header[0] = PIK_CONTROL_FRAME_CONFIG;
    pik_put_u32le(header + 1, 0x0badcafeu);
    put_u16le(header + 5, 1);
    if (!test_encode_frame(header, sizeof(header), payload, sizeof(payload),
                           enc, sizeof(enc), &enc_len))
        return false;
    return test_write_all(fx->master, enc, enc_len);
}

static bool fixture_init_with_stale(ctrl_fixture_t *fx, pik_control_role_t role,
                                    bool stale_input) {
    memset(fx, 0, sizeof(*fx));
    fx->epfd = epoll_create1(EPOLL_CLOEXEC);
    fx->master = -1;
    fx->slave_keepalive = -1;
    fx->peer_session = 0xa5c30001u;
    fx->peer_seq = 0;

    if (fx->epfd < 0) return false;
    if (openpty(&fx->master, &fx->slave_keepalive, fx->slave_name, NULL, NULL) < 0)
        return false;
    test_set_nonblock(fx->master);
    if (stale_input && !write_stale_config(fx))
        return false;

    pik_control_init(fx->epfd, role, command_cb, NULL);
    return pik_control_start(fx->slave_name, pik_now_ms());
}

static bool fixture_init(ctrl_fixture_t *fx, pik_control_role_t role) {
    return fixture_init_with_stale(fx, role, false);
}

static void fixture_cleanup(ctrl_fixture_t *fx) {
    pik_control_cleanup();
    if (fx->master >= 0) close(fx->master);
    if (fx->slave_keepalive >= 0) close(fx->slave_keepalive);
    if (fx->epfd >= 0) close(fx->epfd);
}

static bool dispatch_control(void *ptr, uint32_t events, int64_t now) {
    return pik_control_dispatch(ptr, events, now);
}

static bool read_local_frame(ctrl_fixture_t *fx, uint8_t *enc, size_t enc_cap,
                             size_t *enc_len) {
    for (int i = 0; i < 8; i++) {
        if (test_read_delimited_frame(fx->master, enc, enc_cap, enc_len, 20))
            return true;
        test_epoll_dispatch_one(fx->epfd, dispatch_control, 100);
    }
    return false;
}

static bool decode_control(const uint8_t *enc, size_t enc_len, uint8_t *type,
                           uint32_t *session, uint16_t *seq,
                           pik_frame_t *frame, uint8_t *dec, size_t dec_cap) {
    if (pik_frame_decode(enc, enc_len, 256, PIK_CONTROL_FRAME_HEADER_LEN,
                         dec, dec_cap, frame) != PIK_FRAME_OK)
        return false;
    *type = frame->header[0];
    *session = pik_get_u32le(frame->header + 1);
    *seq = (uint16_t)frame->header[5] | (uint16_t)frame->header[6] << 8;
    return true;
}

static bool send_peer_frame(ctrl_fixture_t *fx, uint8_t type, const uint8_t *payload,
                            size_t payload_len) {
    uint8_t header[PIK_CONTROL_FRAME_HEADER_LEN];
    uint8_t enc[512];
    size_t enc_len = 0;
    header[0] = type;
    pik_put_u32le(header + 1, fx->peer_session);
    put_u16le(header + 5, fx->peer_seq++);
    if (!test_encode_frame(header, sizeof(header), payload, payload_len,
                           enc, sizeof(enc), &enc_len))
        return false;
    return test_write_all(fx->master, enc, enc_len);
}

static bool send_peer_hello(ctrl_fixture_t *fx, pik_control_role_t role,
                            uint32_t protocol) {
    uint8_t p[PIK_CONTROL_HELLO_LEN];
    memset(p, 0, sizeof(p));
    p[0] = PIK_CONTROL_HELLO_MAGIC_0;
    p[1] = PIK_CONTROL_HELLO_MAGIC_1;
    p[2] = PIK_CONTROL_HELLO_MAGIC_2;
    p[3] = PIK_CONTROL_HELLO_MAGIC_3;
    pik_put_u32le(p + 4, protocol);
    pik_put_u32le(p + 8, PIK1_FEATURE_FLAGS);
    p[12] = (uint8_t)role;
    snprintf((char *)p + 13, PIK_CONTROL_RELEASE_LEN, "%s", PIK1_RELEASE_VERSION);
    return send_peer_frame(fx, PIK_CONTROL_FRAME_HELLO, p, sizeof(p));
}

static bool handshake(ctrl_fixture_t *fx) {
    uint8_t enc[512], dec[512], type;
    uint32_t session;
    uint16_t seq;
    size_t enc_len = 0;
    pik_frame_t frame;

    if (!read_local_frame(fx, enc, sizeof(enc), &enc_len)) return false;
    if (!decode_control(enc, enc_len, &type, &session, &seq, &frame, dec, sizeof(dec)))
        return false;
    if (type != PIK_CONTROL_FRAME_HELLO || seq != 0 || session == 0) return false;
    fx->local_session = session;

    if (!send_peer_hello(fx, PIK_CONTROL_ROLE_MCU, PIK1_PROTOCOL_VERSION))
        return false;
    if (!test_epoll_dispatch_one(fx->epfd, dispatch_control, 1000)) return false;
    if (!pik_control_ready()) return false;

    if (!read_local_frame(fx, enc, sizeof(enc), &enc_len)) return false;
    if (!decode_control(enc, enc_len, &type, &session, &seq, &frame, dec, sizeof(dec)))
        return false;
    if (type != PIK_CONTROL_FRAME_HELLO) return false;

    if (!read_local_frame(fx, enc, sizeof(enc), &enc_len)) return false;
    if (!decode_control(enc, enc_len, &type, &session, &seq, &frame, dec, sizeof(dec)))
        return false;
    return type == PIK_CONTROL_FRAME_CONFIG;
}

static void test_successful_handshake_and_ping(void) {
    ctrl_fixture_t fx;
    uint8_t enc[512], dec[512], type;
    uint32_t session;
    uint16_t seq;
    size_t enc_len = 0;
    pik_frame_t frame;

    CHECK(fixture_init(&fx, PIK_CONTROL_ROLE_PTY));
    CHECK(handshake(&fx));
    CHECK(send_peer_frame(&fx, PIK_CONTROL_FRAME_PING, NULL, 0));
    CHECK(test_epoll_dispatch_one(fx.epfd, dispatch_control, 1000));
    CHECK(read_local_frame(&fx, enc, sizeof(enc), &enc_len));
    CHECK(decode_control(enc, enc_len, &type, &session, &seq, &frame, dec, sizeof(dec)));
    CHECK(type == PIK_CONTROL_FRAME_PONG);
    CHECK(frame.payload_len == 0);
    fixture_cleanup(&fx);
}

static void test_protocol_mismatch_fails(void) {
    ctrl_fixture_t fx;
    uint8_t enc[512];
    size_t enc_len = 0;

    CHECK(fixture_init(&fx, PIK_CONTROL_ROLE_PTY));
    CHECK(read_local_frame(&fx, enc, sizeof(enc), &enc_len));
    CHECK(send_peer_hello(&fx, PIK_CONTROL_ROLE_MCU, PIK1_PROTOCOL_VERSION + 1));
    CHECK(!test_epoll_dispatch_one(fx.epfd, dispatch_control, 1000));
    CHECK(!pik_control_ready());
    fixture_cleanup(&fx);
}

static void test_role_mismatch_fails(void) {
    ctrl_fixture_t fx;
    uint8_t enc[512];
    size_t enc_len = 0;

    CHECK(fixture_init(&fx, PIK_CONTROL_ROLE_PTY));
    CHECK(read_local_frame(&fx, enc, sizeof(enc), &enc_len));
    CHECK(send_peer_hello(&fx, PIK_CONTROL_ROLE_PTY, PIK1_PROTOCOL_VERSION));
    CHECK(!test_epoll_dispatch_one(fx.epfd, dispatch_control, 1000));
    fixture_cleanup(&fx);
}

static void test_missing_peer_handshake_times_out(void) {
    ctrl_fixture_t fx;
    uint8_t enc[512];
    size_t enc_len = 0;
    int64_t start;

    CHECK(fixture_init(&fx, PIK_CONTROL_ROLE_PTY));
    start = pik_now_ms();
    CHECK(read_local_frame(&fx, enc, sizeof(enc), &enc_len));
    CHECK(!pik_control_ready());
    CHECK(pik_control_deadline() <= start + 10000);
    CHECK(!pik_control_tick(start + 10001));
    CHECK(!pik_control_ready());
    fixture_cleanup(&fx);
}

static void test_stale_startup_input_is_flushed(void) {
    ctrl_fixture_t fx;

    CHECK(fixture_init_with_stale(&fx, PIK_CONTROL_ROLE_PTY, true));
    CHECK(handshake(&fx));
    fixture_cleanup(&fx);
}

static void test_late_peer_hello_retry_sequence(void) {
    ctrl_fixture_t fx;
    uint8_t enc[512], dec[512], type;
    uint32_t session;
    uint16_t seq;
    size_t enc_len = 0;
    pik_frame_t frame;

    CHECK(fixture_init(&fx, PIK_CONTROL_ROLE_PTY));
    CHECK(read_local_frame(&fx, enc, sizeof(enc), &enc_len));
    fx.peer_seq = 4;
    CHECK(send_peer_hello(&fx, PIK_CONTROL_ROLE_MCU, PIK1_PROTOCOL_VERSION));
    CHECK(test_epoll_dispatch_one(fx.epfd, dispatch_control, 1000));
    CHECK(pik_control_ready());
    CHECK(read_local_frame(&fx, enc, sizeof(enc), &enc_len));
    CHECK(decode_control(enc, enc_len, &type, &session, &seq, &frame, dec, sizeof(dec)));
    CHECK(type == PIK_CONTROL_FRAME_HELLO);
    CHECK(read_local_frame(&fx, enc, sizeof(enc), &enc_len));
    CHECK(decode_control(enc, enc_len, &type, &session, &seq, &frame, dec, sizeof(dec)));
    CHECK(type == PIK_CONTROL_FRAME_CONFIG);
    fixture_cleanup(&fx);
}

static bool send_peer_nak(ctrl_fixture_t *fx, uint16_t expected) {
    uint8_t header[PIK_CONTROL_FRAME_HEADER_LEN];
    uint8_t payload[2] = { (uint8_t)expected, (uint8_t)(expected >> 8) };
    uint8_t enc[128];
    size_t enc_len = 0;
    header[0] = PIK_CONTROL_FRAME_NAK;
    /* a NAK carries the session it is healing: the local side's TX session */
    pik_put_u32le(header + 1, fx->local_session);
    put_u16le(header + 5, fx->peer_seq); /* link-control: seq not consumed */
    if (!test_encode_frame(header, sizeof(header), payload, sizeof(payload),
                           enc, sizeof(enc), &enc_len))
        return false;
    return test_write_all(fx->master, enc, enc_len);
}

static void test_sequence_gap_naks_and_heals(void) {
    ctrl_fixture_t fx;
    uint8_t enc[512], dec[512], type;
    uint32_t session;
    uint16_t seq;
    size_t enc_len = 0;
    pik_frame_t frame;

    CHECK(fixture_init(&fx, PIK_CONTROL_ROLE_PTY));
    CHECK(handshake(&fx));

    /* drop one frame: peer jumps a seq; link must survive and emit a NAK */
    uint16_t lost_seq = fx.peer_seq;
    fx.peer_seq++;
    CHECK(send_peer_frame(&fx, PIK_CONTROL_FRAME_PING, NULL, 0));
    CHECK(test_epoll_dispatch_one(fx.epfd, dispatch_control, 1000));
    CHECK(read_local_frame(&fx, enc, sizeof(enc), &enc_len));
    CHECK(decode_control(enc, enc_len, &type, &session, &seq, &frame, dec, sizeof(dec)));
    CHECK(type == PIK_CONTROL_FRAME_NAK);
    CHECK(frame.payload_len == 2);
    CHECK(((uint16_t)frame.payload[0] | (uint16_t)frame.payload[1] << 8) == lost_seq);

    /* retransmit from the expected seq: both PINGs deliver, both PONGs return */
    fx.peer_seq = lost_seq;
    CHECK(send_peer_frame(&fx, PIK_CONTROL_FRAME_PING, NULL, 0));
    CHECK(send_peer_frame(&fx, PIK_CONTROL_FRAME_PING, NULL, 0));
    CHECK(test_epoll_dispatch_one(fx.epfd, dispatch_control, 1000));
    CHECK(read_local_frame(&fx, enc, sizeof(enc), &enc_len));
    CHECK(decode_control(enc, enc_len, &type, &session, &seq, &frame, dec, sizeof(dec)));
    CHECK(type == PIK_CONTROL_FRAME_PONG);
    CHECK(read_local_frame(&fx, enc, sizeof(enc), &enc_len));
    CHECK(decode_control(enc, enc_len, &type, &session, &seq, &frame, dec, sizeof(dec)));
    CHECK(type == PIK_CONTROL_FRAME_PONG);
    CHECK(pik_control_ready());
    fixture_cleanup(&fx);
}

static void test_sequence_gap_budget_fails(void) {
    ctrl_fixture_t fx;

    CHECK(fixture_init(&fx, PIK_CONTROL_ROLE_PTY));
    CHECK(handshake(&fx));
    fx.peer_seq++;
    CHECK(send_peer_frame(&fx, PIK_CONTROL_FRAME_PING, NULL, 0));
    CHECK(test_epoll_dispatch_one(fx.epfd, dispatch_control, 1000));
    CHECK(pik_control_tick(pik_now_ms()));
    CHECK(!pik_control_tick(pik_now_ms() + 600));
    fixture_cleanup(&fx);
}

static void test_stale_prehello_frames_discarded(void) {
    ctrl_fixture_t fx;

    CHECK(fixture_init(&fx, PIK_CONTROL_ROLE_PTY));
    /* stale non-HELLO traffic from a previous peer session arrives after our
     * link opened: it must be discarded, and the handshake must still work */
    CHECK(write_stale_config(&fx));
    CHECK(test_epoll_dispatch_one(fx.epfd, dispatch_control, 1000));
    CHECK(handshake(&fx));
    fixture_cleanup(&fx);
}

static void test_nak_triggers_retransmit(void) {
    ctrl_fixture_t fx;
    uint8_t enc[512], dec[512], type;
    uint32_t session;
    uint16_t seq;
    size_t enc_len = 0;
    pik_frame_t frame;

    CHECK(fixture_init(&fx, PIK_CONTROL_ROLE_PTY));
    CHECK(handshake(&fx));
    /* local side has sent seq 0..2 (HELLO, HELLO, CONFIG); ask for 2 again */
    CHECK(send_peer_nak(&fx, 2));
    CHECK(test_epoll_dispatch_one(fx.epfd, dispatch_control, 1000));
    CHECK(read_local_frame(&fx, enc, sizeof(enc), &enc_len));
    CHECK(decode_control(enc, enc_len, &type, &session, &seq, &frame, dec, sizeof(dec)));
    CHECK(type == PIK_CONTROL_FRAME_CONFIG);
    CHECK(seq == 2);
    CHECK(pik_control_ready());
    fixture_cleanup(&fx);
}

static void test_nak_beyond_window_fails(void) {
    ctrl_fixture_t fx;

    CHECK(fixture_init(&fx, PIK_CONTROL_ROLE_PTY));
    CHECK(handshake(&fx));
    CHECK(send_peer_nak(&fx, 100)); /* nothing near that in history */
    CHECK(!test_epoll_dispatch_one(fx.epfd, dispatch_control, 1000));
    fixture_cleanup(&fx);
}

static void test_session_change_fails(void) {
    ctrl_fixture_t fx;

    CHECK(fixture_init(&fx, PIK_CONTROL_ROLE_PTY));
    CHECK(handshake(&fx));
    fx.peer_session++;
    CHECK(send_peer_frame(&fx, PIK_CONTROL_FRAME_PING, NULL, 0));
    CHECK(!test_epoll_dispatch_one(fx.epfd, dispatch_control, 1000));
    fixture_cleanup(&fx);
}

static void test_unknown_type_fails(void) {
    ctrl_fixture_t fx;

    CHECK(fixture_init(&fx, PIK_CONTROL_ROLE_PTY));
    CHECK(handshake(&fx));
    CHECK(send_peer_frame(&fx, 0xf0, NULL, 0));
    CHECK(!test_epoll_dispatch_one(fx.epfd, dispatch_control, 1000));
    fixture_cleanup(&fx);
}

static bool send_corrupt_ping(ctrl_fixture_t *fx) {
    uint8_t header[PIK_CONTROL_FRAME_HEADER_LEN];
    uint8_t enc[512];
    size_t enc_len = 0;
    header[0] = PIK_CONTROL_FRAME_PING;
    pik_put_u32le(header + 1, fx->peer_session);
    put_u16le(header + 5, fx->peer_seq++);
    if (!test_encode_frame(header, sizeof(header), NULL, 0,
                           enc, sizeof(enc), &enc_len))
        return false;
    enc[1] ^= 0x40;
    return test_write_all(fx->master, enc, enc_len);
}

static void test_bringup_corrupt_discarded(void) {
    ctrl_fixture_t fx;

    CHECK(fixture_init(&fx, PIK_CONTROL_ROLE_PTY));
    /* torn residue of the peer's previous session arrives before the
     * handshake: it is discarded and the handshake still completes */
    CHECK(send_corrupt_ping(&fx));
    CHECK(test_epoll_dispatch_one(fx.epfd, dispatch_control, 1000));
    fx.peer_seq = 0;
    CHECK(handshake(&fx));
    fixture_cleanup(&fx);
}

static void test_synced_corrupt_frame_heals(void) {
    ctrl_fixture_t fx;
    uint8_t enc[512], dec[512], type;
    uint32_t session;
    uint16_t seq;
    size_t enc_len = 0;
    pik_frame_t frame;

    CHECK(fixture_init(&fx, PIK_CONTROL_ROLE_PTY));
    CHECK(handshake(&fx));

    /* a damaged frame past sync is a lost frame: NAK'd, not fatal */
    uint16_t lost_seq = fx.peer_seq;
    CHECK(send_corrupt_ping(&fx));
    CHECK(test_epoll_dispatch_one(fx.epfd, dispatch_control, 1000));
    CHECK(pik_control_ready());
    CHECK(read_local_frame(&fx, enc, sizeof(enc), &enc_len));
    CHECK(decode_control(enc, enc_len, &type, &session, &seq, &frame, dec, sizeof(dec)));
    CHECK(type == PIK_CONTROL_FRAME_NAK);
    CHECK(frame.payload_len == 2);
    CHECK(((uint16_t)frame.payload[0] | (uint16_t)frame.payload[1] << 8) == lost_seq);

    /* retransmit heals; the stream continues */
    fx.peer_seq = lost_seq;
    CHECK(send_peer_frame(&fx, PIK_CONTROL_FRAME_PING, NULL, 0));
    CHECK(test_epoll_dispatch_one(fx.epfd, dispatch_control, 1000));
    CHECK(read_local_frame(&fx, enc, sizeof(enc), &enc_len));
    CHECK(decode_control(enc, enc_len, &type, &session, &seq, &frame, dec, sizeof(dec)));
    CHECK(type == PIK_CONTROL_FRAME_PONG);
    fixture_cleanup(&fx);
}

static void test_command_callback_and_bad_action_ack(void) {
    ctrl_fixture_t fx;
    uint8_t cmd[5], enc[512], dec[512], type;
    uint32_t session;
    uint16_t seq;
    size_t enc_len = 0;
    pik_frame_t frame;

    command_calls = 0;
    CHECK(fixture_init(&fx, PIK_CONTROL_ROLE_PTY));
    CHECK(handshake(&fx));

    pik_put_u32le(cmd, 0x1234u);
    cmd[4] = PIK_CONTROL_ACTION_STATUS;
    CHECK(send_peer_frame(&fx, PIK_CONTROL_FRAME_COMMAND, cmd, sizeof(cmd)));
    CHECK(test_epoll_dispatch_one(fx.epfd, dispatch_control, 1000));
    CHECK(command_calls == 1);
    CHECK(last_action == PIK_CONTROL_ACTION_STATUS);
    CHECK(last_request == 0x1234u);

    cmd[4] = 0xfeu;
    pik_put_u32le(cmd, 0x5678u);
    CHECK(send_peer_frame(&fx, PIK_CONTROL_FRAME_COMMAND, cmd, sizeof(cmd)));
    CHECK(test_epoll_dispatch_one(fx.epfd, dispatch_control, 1000));
    CHECK(read_local_frame(&fx, enc, sizeof(enc), &enc_len));
    CHECK(decode_control(enc, enc_len, &type, &session, &seq, &frame, dec, sizeof(dec)));
    CHECK(type == PIK_CONTROL_FRAME_ACK);
    CHECK(frame.payload_len == 5);
    CHECK(pik_get_u32le(frame.payload) == 0x5678u);
    CHECK(frame.payload[4] == PIK_CONTROL_ACK_UNKNOWN_ACTION);
    fixture_cleanup(&fx);
}

static void test_bad_ack_status_fails(void) {
    ctrl_fixture_t fx;
    uint8_t ack[5];

    CHECK(fixture_init(&fx, PIK_CONTROL_ROLE_PTY));
    CHECK(handshake(&fx));
    pik_put_u32le(ack, 1);
    ack[4] = 0xffu;
    CHECK(send_peer_frame(&fx, PIK_CONTROL_FRAME_ACK, ack, sizeof(ack)));
    CHECK(!test_epoll_dispatch_one(fx.epfd, dispatch_control, 1000));
    fixture_cleanup(&fx);
}

int main(void) {
    test_successful_handshake_and_ping();
    test_protocol_mismatch_fails();
    test_role_mismatch_fails();
    test_missing_peer_handshake_times_out();
    test_stale_startup_input_is_flushed();
    test_late_peer_hello_retry_sequence();
    test_sequence_gap_naks_and_heals();
    test_sequence_gap_budget_fails();
    test_stale_prehello_frames_discarded();
    test_nak_triggers_retransmit();
    test_nak_beyond_window_fails();
    test_session_change_fails();
    test_unknown_type_fails();
    test_bringup_corrupt_discarded();
    test_synced_corrupt_frame_heals();
    test_command_callback_and_bad_action_ack();
    test_bad_ack_status_fails();

    if (failures) {
        fprintf(stderr, "test_control_protocol: %d failure(s)\n", failures);
        return 1;
    }
    puts("test_control_protocol: ok");
    return 0;
}
