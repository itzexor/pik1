#include "control.h"
#include "test_harness.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures;

static bool stub_ack_ok = true;
static int stub_ack_calls;
static uint32_t stub_ack_request;
static pik_control_ack_status_t stub_ack_status;
static uint8_t stub_ack_payload[256];
static size_t stub_ack_payload_len;
static bool stub_peer_known;
static uint32_t stub_peer_flags;
static int64_t stub_now = 100000;

bool test_pik_control_send_ack(uint32_t request_id, pik_control_ack_status_t status,
                               const uint8_t *payload, size_t payload_len);
bool test_pik_control_peer_link_state(uint32_t *flags);
int64_t test_pik_now_ms(void);

#define main pik1d_main_unused
#define pik_control_send_ack test_pik_control_send_ack
#define pik_control_peer_link_state test_pik_control_peer_link_state
#define pik_now_ms test_pik_now_ms
#include "../src/pik1d.c"
#undef pik_now_ms
#undef pik_control_peer_link_state
#undef pik_control_send_ack
#undef main

bool test_pik_control_send_ack(uint32_t request_id, pik_control_ack_status_t status,
                               const uint8_t *payload, size_t payload_len) {
    stub_ack_calls++;
    stub_ack_request = request_id;
    stub_ack_status = status;
    if (payload_len > sizeof(stub_ack_payload))
        payload_len = sizeof(stub_ack_payload);
    stub_ack_payload_len = payload_len;
    if (payload_len)
        memcpy(stub_ack_payload, payload, payload_len);
    return stub_ack_ok;
}

bool test_pik_control_peer_link_state(uint32_t *flags) {
    if (!stub_peer_known) return false;
    if (flags) *flags = stub_peer_flags;
    return true;
}

int64_t test_pik_now_ms(void) {
    return stub_now;
}

static void reset_state(void) {
    stub_ack_ok = true;
    stub_ack_calls = 0;
    stub_ack_request = 0;
    stub_ack_status = 0;
    stub_ack_payload_len = 0;
    stub_peer_known = false;
    stub_peer_flags = 0;
    stub_now = 100000;

    g_uart_name = "test";
    g_link_flags = 0;
    g_remote_action = 0;
    g_remote_action_pending = false;
    g_remote_action_at_ms = 0;
}

static void test_status_ack_payload(void) {
    reset_state();
    g_link_flags = PIK_CONTROL_LINK_SERIAL;
    stub_peer_known = true;
    stub_peer_flags = PIK_CONTROL_LINK_TCP;

    on_control_command(PIK_CONTROL_ACTION_STATUS, 77, NULL);

    CHECK(stub_ack_calls == 1);
    CHECK(stub_ack_request == 77);
    CHECK(stub_ack_status == PIK_CONTROL_ACK_OK);
    CHECK(stub_ack_payload_len > 0);
    CHECK(memmem(stub_ack_payload, stub_ack_payload_len, "links=serial", 12) != NULL);
    CHECK(memmem(stub_ack_payload, stub_ack_payload_len, "peer=tcp", 8) != NULL);
    CHECK(!g_remote_action_pending);
}

static void test_remote_action_requires_ack_success(void) {
    reset_state();

    on_control_command(PIK_CONTROL_ACTION_REBOOT_PEER, 88, NULL);

    CHECK(stub_ack_calls == 1);
    CHECK(stub_ack_status == PIK_CONTROL_ACK_OK);
    CHECK(g_remote_action_pending);
    CHECK(g_remote_action == PIK_CONTROL_ACTION_REBOOT_PEER);
    CHECK(g_remote_action_at_ms == stub_now + REMOTE_ACTION_DELAY_MS);
}

static void test_remote_action_not_scheduled_on_ack_failure(void) {
    reset_state();
    stub_ack_ok = false;

    on_control_command(PIK_CONTROL_ACTION_POWEROFF_PEER, 99, NULL);

    CHECK(stub_ack_calls == 1);
    CHECK(!g_remote_action_pending);
    CHECK(g_remote_action_at_ms == 0);
}

int main(void) {
    test_status_ack_payload();
    test_remote_action_requires_ack_success();
    test_remote_action_not_scheduled_on_ack_failure();

    if (failures) {
        fprintf(stderr, "test_pik1d_supervisor: %d failure(s)\n", failures);
        return 1;
    }
    puts("test_pik1d_supervisor: ok");
    return 0;
}
