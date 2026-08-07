#include "test_session_harness.h"

static int failures;

static int command_calls;
static pik_control_action_t last_action;
static uint32_t last_request;

static void command_cb(pik_control_action_t action, uint32_t request_id) {
    command_calls++;
    last_action = action;
    last_request = request_id;
}

static bool fixture_init_with_stale(session_fixture_t *fx, pik_control_role_t role,
                                    bool stale_input) {
    session_fixture_cfg_t cfg = {
        .role = role,
        .on_command = command_cb,
        .stale_input = stale_input,
    };
    return sfx_init(fx, &cfg);
}

static bool fixture_init(session_fixture_t *fx, pik_control_role_t role) {
    return fixture_init_with_stale(fx, role, false);
}

static void test_successful_handshake_and_ping(void) {
    session_fixture_t fx;
    sfx_frame_t f;

    CHECK(fixture_init(&fx, PIK_CONTROL_ROLE_PTY));
    CHECK(sfx_handshake(&fx));
    CHECK(sfx_ready_calls == 1);
    CHECK(sfx_send_peer_frame(&fx, PIK_FRAME_CTRL_PING, PIK_CH_CONTROL, NULL, 0));
    CHECK(sfx_dispatch_one(&fx, 1000));
    CHECK(sfx_read_frame(&fx, &f));
    CHECK(f.type == PIK_FRAME_CTRL_PONG);
    CHECK(f.ch == PIK_CH_CONTROL);
    CHECK(f.payload_len == 0);
    sfx_cleanup(&fx);
}

static void test_protocol_mismatch_fails(void) {
    session_fixture_t fx;
    sfx_frame_t f;

    CHECK(fixture_init(&fx, PIK_CONTROL_ROLE_PTY));
    CHECK(sfx_read_frame(&fx, &f));
    CHECK(sfx_send_peer_hello(&fx, PIK_CONTROL_ROLE_MCU, PIK1_PROTOCOL_VERSION + 1));
    CHECK(!sfx_dispatch_one(&fx, 1000));
    CHECK(sfx_ready_calls == 0);
    sfx_cleanup(&fx);
}

static void test_role_mismatch_fails(void) {
    session_fixture_t fx;
    sfx_frame_t f;

    CHECK(fixture_init(&fx, PIK_CONTROL_ROLE_PTY));
    CHECK(sfx_read_frame(&fx, &f));
    CHECK(sfx_send_peer_hello(&fx, PIK_CONTROL_ROLE_PTY, PIK1_PROTOCOL_VERSION));
    CHECK(!sfx_dispatch_one(&fx, 1000));
    sfx_cleanup(&fx);
}

static void test_invalid_peer_role_fails(void) {
    session_fixture_t fx;
    sfx_frame_t f;

    CHECK(fixture_init(&fx, PIK_CONTROL_ROLE_PTY));
    CHECK(sfx_read_frame(&fx, &f));
    CHECK(sfx_send_peer_hello(&fx, (pik_control_role_t)0,
                              PIK1_PROTOCOL_VERSION));
    CHECK(!sfx_dispatch_one(&fx, 1000));
    sfx_cleanup(&fx);
}

static void test_missing_peer_handshake_times_out(void) {
    session_fixture_t fx;
    sfx_frame_t f;
    int64_t start;

    CHECK(fixture_init(&fx, PIK_CONTROL_ROLE_PTY));
    start = pik_session_link()->last_rx_ms;
    CHECK(sfx_read_frame(&fx, &f));
    {
        uint8_t empty_frames[] = { 0, 0, 0 };
        CHECK(pik_link_feed(pik_session_link(), empty_frames,
                            sizeof(empty_frames), start + 9000));
    }
    CHECK(sfx_ready_calls == 0);
    CHECK(pik_control_deadline() <= start + 10000);
    CHECK(!pik_control_tick(start + 10001));
    CHECK(sfx_ready_calls == 0);
    sfx_cleanup(&fx);
}

static void test_stale_startup_input_is_flushed(void) {
    session_fixture_t fx;

    CHECK(fixture_init_with_stale(&fx, PIK_CONTROL_ROLE_PTY, true));
    CHECK(sfx_handshake(&fx));
    sfx_cleanup(&fx);
}

static void test_late_peer_hello_retry_sequence(void) {
    session_fixture_t fx;
    sfx_frame_t f;

    CHECK(fixture_init(&fx, PIK_CONTROL_ROLE_PTY));
    CHECK(sfx_read_frame(&fx, &f));
    fx.peer_seq = 4;
    CHECK(sfx_send_peer_hello(&fx, PIK_CONTROL_ROLE_MCU, PIK1_PROTOCOL_VERSION));
    CHECK(sfx_dispatch_one(&fx, 1000));
    CHECK(sfx_ready_calls == 1);
    CHECK(sfx_read_frame(&fx, &f));
    CHECK(f.type == PIK_FRAME_CTRL_HELLO);
    CHECK(sfx_read_frame(&fx, &f));
    CHECK(f.type == PIK_FRAME_CTRL_CONFIG);
    sfx_cleanup(&fx);
}

static void test_recovered_handshake_clears_retry_quiet(void) {
    session_fixture_t fx;
    sfx_frame_t f;
    int64_t start;

    CHECK(fixture_init(&fx, PIK_CONTROL_ROLE_PTY));
    start = pik_session_link()->last_rx_ms;
    CHECK(sfx_read_frame(&fx, &f));
    CHECK(!pik_control_tick(start + 10001));
    CHECK(pik_control_handshake_failures() == 1);

    /* Mirrors pik1d starting a transport retry after the first timeout. */
    pik_session_cleanup();
    pik_session_link()->quiet = true;
    pik_link_begin(pik_session_link(), start + 11001);
    CHECK(pik_control_on_link_open());
    CHECK(pik_session_link()->quiet);
    CHECK(sfx_read_frame(&fx, &f));
    CHECK(sfx_send_peer_hello(&fx, PIK_CONTROL_ROLE_MCU,
                              PIK1_PROTOCOL_VERSION));
    CHECK(sfx_dispatch_one(&fx, 1000));
    CHECK(!pik_session_link()->quiet);
    CHECK(pik_control_handshake_failures() == 0);
    sfx_cleanup(&fx);
}

static void test_sequence_gap_naks_and_heals(void) {
    session_fixture_t fx;
    sfx_frame_t f;

    CHECK(fixture_init(&fx, PIK_CONTROL_ROLE_PTY));
    CHECK(sfx_handshake(&fx));

    /* drop one frame: peer jumps a seq; link must survive and emit a NAK */
    uint16_t lost_seq = fx.peer_seq;
    fx.peer_seq++;
    CHECK(sfx_send_peer_frame(&fx, PIK_FRAME_CTRL_PING, PIK_CH_CONTROL, NULL, 0));
    CHECK(sfx_dispatch_one(&fx, 1000));
    CHECK(sfx_read_frame(&fx, &f));
    CHECK(f.type == PIK_FRAME_NAK);
    CHECK(f.payload_len == 2);
    CHECK(((uint16_t)f.payload[0] | (uint16_t)f.payload[1] << 8) == lost_seq);

    /* retransmit from the expected seq: both PINGs deliver, both PONGs return */
    fx.peer_seq = lost_seq;
    CHECK(sfx_send_peer_frame(&fx, PIK_FRAME_CTRL_PING, PIK_CH_CONTROL, NULL, 0));
    CHECK(sfx_send_peer_frame(&fx, PIK_FRAME_CTRL_PING, PIK_CH_CONTROL, NULL, 0));
    CHECK(sfx_dispatch_one(&fx, 1000));
    CHECK(sfx_read_frame(&fx, &f));
    CHECK(f.type == PIK_FRAME_CTRL_PONG);
    CHECK(sfx_read_frame(&fx, &f));
    CHECK(f.type == PIK_FRAME_CTRL_PONG);
    CHECK(sfx_ready_calls == 1);
    sfx_cleanup(&fx);
}

static void test_sequence_gap_budget_fails(void) {
    session_fixture_t fx;

    CHECK(fixture_init(&fx, PIK_CONTROL_ROLE_PTY));
    CHECK(sfx_handshake(&fx));
    fx.peer_seq++;
    CHECK(sfx_send_peer_frame(&fx, PIK_FRAME_CTRL_PING, PIK_CH_CONTROL, NULL, 0));
    CHECK(sfx_dispatch_one(&fx, 1000));
    CHECK(sfx_tick(pik_now_ms()));
    CHECK(!sfx_tick(pik_now_ms() + 600));
    sfx_cleanup(&fx);
}

static void test_stale_prehello_frames_discarded(void) {
    session_fixture_t fx;

    CHECK(fixture_init(&fx, PIK_CONTROL_ROLE_PTY));
    /* stale non-HELLO traffic from a previous peer session arrives after our
     * link opened: it must be discarded, and the handshake must still work */
    {
        uint8_t payload[] = { PIK_CONTROL_TCP_NONE, 0 };
    CHECK(sfx_write_frame_hdr(&fx, PIK_FRAME_CTRL_CONFIG, PIK_CH_CONTROL,
                                  0x0badcafeu, 1, payload, sizeof(payload)));
    }
    CHECK(sfx_settle(&fx));
    CHECK(sfx_handshake(&fx));
    sfx_cleanup(&fx);
}

static void test_nak_triggers_retransmit(void) {
    session_fixture_t fx;
    sfx_frame_t f;

    CHECK(fixture_init(&fx, PIK_CONTROL_ROLE_PTY));
    CHECK(sfx_handshake(&fx));
    /* local side has sent seq 0..2 (HELLO, HELLO, CONFIG); ask for 2 again */
    CHECK(sfx_send_peer_nak(&fx, 2));
    CHECK(sfx_dispatch_one(&fx, 1000));
    CHECK(sfx_read_frame(&fx, &f));
    CHECK(f.type == PIK_FRAME_CTRL_CONFIG);
    CHECK(f.seq == 2);
    CHECK(sfx_ready_calls == 1);
    sfx_cleanup(&fx);
}

static void test_nak_beyond_window_fails(void) {
    session_fixture_t fx;

    CHECK(fixture_init(&fx, PIK_CONTROL_ROLE_PTY));
    CHECK(sfx_handshake(&fx));
    CHECK(sfx_send_peer_nak(&fx, 100)); /* nothing near that in history */
    CHECK(!sfx_dispatch_one(&fx, 1000));
    sfx_cleanup(&fx);
}

static void test_session_change_fails(void) {
    session_fixture_t fx;

    CHECK(fixture_init(&fx, PIK_CONTROL_ROLE_PTY));
    CHECK(sfx_handshake(&fx));
    fx.peer_session++;
    CHECK(sfx_send_peer_frame(&fx, PIK_FRAME_CTRL_PING, PIK_CH_CONTROL, NULL, 0));
    CHECK(!sfx_dispatch_one(&fx, 1000));
    sfx_cleanup(&fx);
}

static void test_unknown_type_fails(void) {
    session_fixture_t fx;

    CHECK(fixture_init(&fx, PIK_CONTROL_ROLE_PTY));
    CHECK(sfx_handshake(&fx));
    CHECK(sfx_send_peer_frame(&fx, 0xf0, PIK_CH_CONTROL, NULL, 0));
    CHECK(!sfx_dispatch_one(&fx, 1000));
    sfx_cleanup(&fx);
}

static void test_control_frame_on_bad_channel_fails(void) {
    session_fixture_t fx;

    CHECK(fixture_init(&fx, PIK_CONTROL_ROLE_PTY));
    CHECK(sfx_handshake(&fx));
    /* control-namespace type on a data channel: the router must reject it */
    CHECK(sfx_send_peer_frame(&fx, PIK_FRAME_CTRL_PING, 3, NULL, 0));
    CHECK(!sfx_dispatch_one(&fx, 1000));
    sfx_cleanup(&fx);
}

static bool send_corrupt_ping(session_fixture_t *fx) {
    uint8_t header[PIK_FRAME_HEADER_LEN];
    uint8_t enc[512];
    size_t enc_len = 0;
    header[0] = PIK_FRAME_CTRL_PING;
    header[1] = PIK_CH_CONTROL;
    pik_put_u32le(header + 2, fx->peer_session);
    sfx_put_u16le(header + 6, fx->peer_seq++);
    if (!test_encode_frame(header, NULL, 0,
                           enc, sizeof(enc), &enc_len))
        return false;
    enc[1] ^= 0x40;
    return sfx_feed_encoded(enc, enc_len);
}

static void test_bringup_corrupt_discarded(void) {
    session_fixture_t fx;

    CHECK(fixture_init(&fx, PIK_CONTROL_ROLE_PTY));
    /* torn residue of the peer's previous session arrives before the
     * handshake: it is discarded and the handshake still completes */
    CHECK(send_corrupt_ping(&fx));
    CHECK(sfx_settle(&fx));
    fx.peer_seq = 0;
    CHECK(sfx_handshake(&fx));
    sfx_cleanup(&fx);
}

static void test_synced_corrupt_frame_heals(void) {
    session_fixture_t fx;
    sfx_frame_t f;

    CHECK(fixture_init(&fx, PIK_CONTROL_ROLE_PTY));
    CHECK(sfx_handshake(&fx));

    /* a damaged frame past sync is a lost frame: NAK'd, not fatal */
    uint16_t lost_seq = fx.peer_seq;
    CHECK(send_corrupt_ping(&fx));
    CHECK(sfx_dispatch_one(&fx, 1000));
    CHECK(sfx_ready_calls == 1);
    CHECK(sfx_read_frame(&fx, &f));
    CHECK(f.type == PIK_FRAME_NAK);
    CHECK(f.payload_len == 2);
    CHECK(((uint16_t)f.payload[0] | (uint16_t)f.payload[1] << 8) == lost_seq);

    /* retransmit heals; the stream continues */
    fx.peer_seq = lost_seq;
    CHECK(sfx_send_peer_frame(&fx, PIK_FRAME_CTRL_PING, PIK_CH_CONTROL, NULL, 0));
    CHECK(sfx_dispatch_one(&fx, 1000));
    CHECK(sfx_read_frame(&fx, &f));
    CHECK(f.type == PIK_FRAME_CTRL_PONG);
    sfx_cleanup(&fx);
}

static void test_command_callback_and_bad_action_ack(void) {
    session_fixture_t fx;
    sfx_frame_t f;
    uint8_t cmd[5];

    command_calls = 0;
    CHECK(fixture_init(&fx, PIK_CONTROL_ROLE_PTY));
    CHECK(sfx_handshake(&fx));

    pik_put_u32le(cmd, 0x1234u);
    cmd[4] = PIK_CONTROL_ACTION_STATUS;
    CHECK(sfx_send_peer_frame(&fx, PIK_FRAME_CTRL_COMMAND, PIK_CH_CONTROL,
                              cmd, sizeof(cmd)));
    CHECK(sfx_settle(&fx));
    CHECK(command_calls == 1);
    CHECK(last_action == PIK_CONTROL_ACTION_STATUS);
    CHECK(last_request == 0x1234u);

    cmd[4] = PIK_CONTROL_ACTION_RESTART_WIFI;
    pik_put_u32le(cmd, 0x3456u);
    CHECK(sfx_send_peer_frame(&fx, PIK_FRAME_CTRL_COMMAND, PIK_CH_CONTROL,
                              cmd, sizeof(cmd)));
    CHECK(sfx_settle(&fx));
    CHECK(command_calls == 2);
    CHECK(last_action == PIK_CONTROL_ACTION_RESTART_WIFI);
    CHECK(last_request == 0x3456u);

    cmd[4] = PIK_CONTROL_ACTION_RESTART_KLIPPER;
    pik_put_u32le(cmd, 0x4567u);
    CHECK(sfx_send_peer_frame(&fx, PIK_FRAME_CTRL_COMMAND, PIK_CH_CONTROL,
                              cmd, sizeof(cmd)));
    CHECK(sfx_settle(&fx));
    CHECK(command_calls == 3);
    CHECK(last_action == PIK_CONTROL_ACTION_RESTART_KLIPPER);
    CHECK(last_request == 0x4567u);

    cmd[4] = 0xfeu;
    pik_put_u32le(cmd, 0x5678u);
    CHECK(sfx_send_peer_frame(&fx, PIK_FRAME_CTRL_COMMAND, PIK_CH_CONTROL,
                              cmd, sizeof(cmd)));
    CHECK(sfx_dispatch_one(&fx, 1000));
    CHECK(sfx_read_frame(&fx, &f));
    CHECK(f.type == PIK_FRAME_CTRL_ACK);
    CHECK(f.payload_len == 5);
    CHECK(pik_get_u32le(f.payload) == 0x5678u);
    CHECK(f.payload[4] == PIK_CONTROL_ACK_UNKNOWN_ACTION);
    sfx_cleanup(&fx);
}

static void test_bad_ack_status_fails(void) {
    session_fixture_t fx;
    uint8_t ack[5];

    CHECK(fixture_init(&fx, PIK_CONTROL_ROLE_PTY));
    CHECK(sfx_handshake(&fx));
    pik_put_u32le(ack, 1);
    ack[4] = 0xffu;
    CHECK(sfx_send_peer_frame(&fx, PIK_FRAME_CTRL_ACK, PIK_CH_CONTROL,
                              ack, sizeof(ack)));
    CHECK(!sfx_dispatch_one(&fx, 1000));
    sfx_cleanup(&fx);
}

static void test_oversized_ack_fails(void) {
    session_fixture_t fx;
    uint8_t ack[PIK_CTRL_MAX_PAYLOAD + 1u] = { 0 };

    CHECK(fixture_init(&fx, PIK_CONTROL_ROLE_PTY));
    CHECK(sfx_handshake(&fx));
    pik_put_u32le(ack, 1);
    ack[4] = PIK_CONTROL_ACK_OK;
    CHECK(sfx_send_peer_frame(&fx, PIK_FRAME_CTRL_ACK, PIK_CH_CONTROL,
                              ack, sizeof(ack)));
    CHECK(!sfx_dispatch_one(&fx, 1000));
    sfx_cleanup(&fx);
}

static void test_unconsumed_ack_cannot_be_overwritten(void) {
    session_fixture_t fx;
    uint8_t ack[5] = { 0 };

    CHECK(fixture_init(&fx, PIK_CONTROL_ROLE_PTY));
    CHECK(sfx_handshake(&fx));
    pik_put_u32le(ack, 1);
    ack[4] = PIK_CONTROL_ACK_OK;
    CHECK(sfx_send_peer_frame(&fx, PIK_FRAME_CTRL_ACK, PIK_CH_CONTROL,
                              ack, sizeof(ack)));
    CHECK(pik_session_up());

    pik_put_u32le(ack, 2);
    CHECK(sfx_send_peer_frame(&fx, PIK_FRAME_CTRL_ACK, PIK_CH_CONTROL,
                              ack, sizeof(ack)));
    CHECK(!pik_session_up());
    sfx_cleanup(&fx);
}

static void test_invalid_config_channel_fails(void) {
    session_fixture_t fx;
    uint8_t config[] = { PIK_CONTROL_TCP_NONE, 1, PIK_MUX_CLI_LAST + 1u };

    CHECK(fixture_init(&fx, PIK_CONTROL_ROLE_PTY));
    CHECK(sfx_handshake(&fx));
    CHECK(sfx_send_peer_frame(&fx, PIK_FRAME_CTRL_CONFIG, PIK_CH_CONTROL,
                              config, sizeof(config)));
    CHECK(!pik_session_up());
    sfx_cleanup(&fx);
}

int main(void) {
    test_successful_handshake_and_ping();
    test_protocol_mismatch_fails();
    test_role_mismatch_fails();
    test_invalid_peer_role_fails();
    test_missing_peer_handshake_times_out();
    test_stale_startup_input_is_flushed();
    test_late_peer_hello_retry_sequence();
    test_recovered_handshake_clears_retry_quiet();
    test_sequence_gap_naks_and_heals();
    test_sequence_gap_budget_fails();
    test_stale_prehello_frames_discarded();
    test_nak_triggers_retransmit();
    test_nak_beyond_window_fails();
    test_session_change_fails();
    test_unknown_type_fails();
    test_control_frame_on_bad_channel_fails();
    test_bringup_corrupt_discarded();
    test_synced_corrupt_frame_heals();
    test_command_callback_and_bad_action_ack();
    test_bad_ack_status_fails();
    test_oversized_ack_fails();
    test_unconsumed_ack_cannot_be_overwritten();
    test_invalid_config_channel_fails();

    if (failures) {
        fprintf(stderr, "test_control_protocol: %d failure(s)\n", failures);
        return 1;
    }
    puts("test_control_protocol: ok");
    return 0;
}
