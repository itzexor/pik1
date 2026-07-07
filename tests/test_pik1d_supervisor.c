#include "control.h"
#include "test_harness.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

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
bool test_pik_control_send_command(pik_control_action_t action, uint32_t *request_id);
bool test_pik_control_take_ack(uint32_t *request_id, pik_control_ack_status_t *status,
                               const uint8_t **payload, size_t *payload_len);
int64_t test_pik_now_ms(void);

#define main pik1d_main_unused
#define pik_control_send_ack test_pik_control_send_ack
#define pik_control_peer_link_state test_pik_control_peer_link_state
#define pik_control_send_command test_pik_control_send_command
#define pik_control_take_ack test_pik_control_take_ack
#define LOCAL_CONTROL_DIR "/tmp/pik1-test-run"
#define pik_now_ms test_pik_now_ms
#include "../src/pik1d.c"
#undef pik_now_ms
#undef pik_control_take_ack
#undef pik_control_send_command
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

static bool stub_send_command_ok = true;
static int stub_send_command_calls;
static pik_control_action_t stub_sent_action;
static uint32_t stub_next_request_id = 1000;
static bool stub_ack_available;
static uint32_t stub_take_ack_request;
static pik_control_ack_status_t stub_take_ack_status;
static const char *stub_take_ack_payload;

bool test_pik_control_send_command(pik_control_action_t action, uint32_t *request_id) {
    stub_send_command_calls++;
    stub_sent_action = action;
    if (!stub_send_command_ok) return false;
    if (request_id) *request_id = stub_next_request_id;
    return true;
}

bool test_pik_control_take_ack(uint32_t *request_id, pik_control_ack_status_t *status,
                               const uint8_t **payload, size_t *payload_len) {
    if (!stub_ack_available) return false;
    stub_ack_available = false;
    if (request_id) *request_id = stub_take_ack_request;
    if (status) *status = stub_take_ack_status;
    if (payload) *payload = (const uint8_t *)stub_take_ack_payload;
    if (payload_len)
        *payload_len = stub_take_ack_payload ? strlen(stub_take_ack_payload) : 0;
    return true;
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

    stub_send_command_ok = true;
    stub_send_command_calls = 0;
    stub_sent_action = 0;
    stub_next_request_id = 1000;
    stub_ack_available = false;
    stub_take_ack_request = 0;
    stub_take_ack_status = 0;
    stub_take_ack_payload = NULL;

    g_uart_name = "test";
    g_link_flags = 0;
    g_remote_action = 0;
    g_remote_action_pending = false;
    g_remote_action_at_ms = 0;
    g_child.epfd = -1;
    g_child.status_fd = -1;
    g_local.listen_fd = -1;
    g_local.client_fd = -1;
    g_local.epfd = -1;
    g_local.command_pending = false;
    g_local.request_id = 0;
    g_local.deadline_ms = 0;
    g_signal_command_pending = false;
    g_signal_command_done = false;
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

static void test_child_status_updates_tcp_link(void) {
    reset_state();

    int fds[2];
    CHECK(pipe(fds) == 0);
    int flags = fcntl(fds[0], F_GETFL, 0);
    CHECK(flags >= 0);
    CHECK(fcntl(fds[0], F_SETFL, flags | O_NONBLOCK) == 0);
    g_child.status_fd = fds[0];

    CHECK(write(fds[1], "U", 1) == 1);
    child_status_read();
    CHECK((g_link_flags & PIK_CONTROL_LINK_TCP) != 0);

    CHECK(write(fds[1], "D", 1) == 1);
    child_status_read();
    CHECK((g_link_flags & PIK_CONTROL_LINK_TCP) == 0);

    close(fds[0]);
    close(fds[1]);
    g_child.status_fd = -1;
}

static void test_parse_control_actions(void) {
    pik_control_action_t action = 0;
    CHECK(parse_control_action("status-peer", &action));
    CHECK(action == PIK_CONTROL_ACTION_STATUS);
    CHECK(parse_control_action("poweroff-peer", &action));
    CHECK(action == PIK_CONTROL_ACTION_POWEROFF_PEER);
    CHECK(!parse_control_action("thumbnail", &action));
    CHECK(!parse_control_action("bogus", &action));
}

/* attach a fake local client via socketpair; returns the peer end */
static int attach_local_client(int epfd) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv) < 0) return -1;
    g_local.epfd = epfd;
    g_local.client_fd = sv[0];
    return sv[1];
}

static bool read_reply(int fd, char *buf, size_t cap) {
    ssize_t n;
    do {
        n = read(fd, buf, cap - 1);
    } while (n < 0 && errno == EINTR);
    if (n <= 0) return false;
    buf[n] = '\0';
    return true;
}

static void test_local_command_roundtrip(void) {
    char reply[128];
    reset_state();
    int epfd = epoll_create1(0);
    int peer = attach_local_client(epfd);
    CHECK(peer >= 0);

    CHECK(write(peer, "status-peer\n", 12) == 12);
    local_control_read(stub_now);
    CHECK(stub_send_command_calls == 1);
    CHECK(stub_sent_action == PIK_CONTROL_ACTION_STATUS);
    CHECK(g_local.command_pending);
    CHECK(g_local.deadline_ms == stub_now + COMMAND_ACK_TIMEOUT_MS);

    stub_ack_available = true;
    stub_take_ack_request = 1000;
    stub_take_ack_status = PIK_CONTROL_ACK_OK;
    stub_take_ack_payload = "uart=pty links=serial";
    local_control_check_ack(stub_now);
    CHECK(read_reply(peer, reply, sizeof(reply)));
    CHECK(strcmp(reply, "OK uart=pty links=serial\n") == 0);
    CHECK(!g_local.command_pending);
    close(peer);
    close(epfd);
}

static void test_local_error_ack_includes_reason(void) {
    char reply[128];
    reset_state();
    int epfd = epoll_create1(0);
    int peer = attach_local_client(epfd);
    CHECK(peer >= 0);

    CHECK(write(peer, "reboot-peer\n", 12) == 12);
    local_control_read(stub_now);
    stub_ack_available = true;
    stub_take_ack_request = 1000;
    stub_take_ack_status = PIK_CONTROL_ACK_INTERNAL_ERROR;
    stub_take_ack_payload = "exec failed";
    local_control_check_ack(stub_now);
    CHECK(read_reply(peer, reply, sizeof(reply)));
    CHECK(strcmp(reply, "ERR peer internal-error exec failed\n") == 0);
    close(peer);
    close(epfd);
}

static void test_local_busy_while_signal_pending(void) {
    char reply[64];
    reset_state();
    int epfd = epoll_create1(0);
    int peer = attach_local_client(epfd);
    CHECK(peer >= 0);

    g_signal_command_pending = true;
    CHECK(write(peer, "status-peer\n", 12) == 12);
    local_control_read(stub_now);
    CHECK(stub_send_command_calls == 0);
    CHECK(read_reply(peer, reply, sizeof(reply)));
    CHECK(strcmp(reply, "ERR busy\n") == 0);
    close(peer);
    close(epfd);
}

static void test_local_ack_timeout(void) {
    char reply[64];
    reset_state();
    int epfd = epoll_create1(0);
    int peer = attach_local_client(epfd);
    CHECK(peer >= 0);

    CHECK(write(peer, "status-peer\n", 12) == 12);
    local_control_read(stub_now);
    local_control_check_ack(stub_now + COMMAND_ACK_TIMEOUT_MS);
    CHECK(read_reply(peer, reply, sizeof(reply)));
    CHECK(strcmp(reply, "ERR peer ack timeout\n") == 0);
    CHECK(!g_local.command_pending);
    close(peer);
    close(epfd);
}

static void test_peer_initiated_marker(void) {
    reset_state();
    system("rm -rf " LOCAL_CONTROL_DIR);
    CHECK(mkdir(LOCAL_CONTROL_DIR, 0700) == 0);
    CHECK(access(PEER_INITIATED_MARKER, F_OK) != 0);
    mark_peer_initiated();
    CHECK(access(PEER_INITIATED_MARKER, F_OK) == 0);
    system("rm -rf " LOCAL_CONTROL_DIR);
}

static void test_local_control_start_creates_dir(void) {
    reset_state();
    unsetenv(CONTROL_SOCK_ENV);
    system("rm -rf " LOCAL_CONTROL_DIR);
    int epfd = epoll_create1(0);

    CHECK(local_control_start(epfd));
    CHECK(access(LOCAL_CONTROL_SOCK, F_OK) == 0);
    local_control_cleanup();
    CHECK(access(LOCAL_CONTROL_SOCK, F_OK) != 0);

    /* second start with the directory already present (EEXIST path) */
    CHECK(local_control_start(epfd));
    local_control_cleanup();
    system("rm -rf " LOCAL_CONTROL_DIR);
    close(epfd);
}

int main(void) {
    test_status_ack_payload();
    test_remote_action_requires_ack_success();
    test_remote_action_not_scheduled_on_ack_failure();
    test_child_status_updates_tcp_link();
    test_parse_control_actions();
    test_local_command_roundtrip();
    test_local_error_ack_includes_reason();
    test_local_busy_while_signal_pending();
    test_local_ack_timeout();
    test_peer_initiated_marker();
    test_local_control_start_creates_dir();

    if (failures) {
        fprintf(stderr, "test_pik1d_supervisor: %d failure(s)\n", failures);
        return 1;
    }
    puts("test_pik1d_supervisor: ok");
    return 0;
}
