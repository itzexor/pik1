#include "commands.h"
#include "test_harness.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

static int failures;

static int64_t now_ms = 100000;

static bool send_ack_ok = true;
static int send_ack_calls;
static uint32_t send_ack_request;
static pik_control_ack_status_t send_ack_status;
static uint8_t send_ack_payload[4096];
static size_t send_ack_payload_len;

static bool send_command_ok = true;
static int send_command_calls;
static pik_control_action_t sent_action;
static uint32_t next_request_id = 1000;

static bool ack_available;
static uint32_t ack_request;
static pik_control_ack_status_t ack_status;
static const uint8_t *ack_payload;
static size_t ack_payload_len;

int64_t pik_now_ms(void) {
    return now_ms;
}

bool pik_control_send_ack(uint32_t request_id, pik_control_ack_status_t status,
                          const uint8_t *payload, size_t payload_len) {
    send_ack_calls++;
    send_ack_request = request_id;
    send_ack_status = status;
    if (payload_len > sizeof(send_ack_payload))
        payload_len = sizeof(send_ack_payload);
    send_ack_payload_len = payload_len;
    if (payload_len)
        memcpy(send_ack_payload, payload, payload_len);
    return send_ack_ok;
}

bool pik_control_send_command(pik_control_action_t action, uint32_t *request_id) {
    send_command_calls++;
    sent_action = action;
    if (!send_command_ok) return false;
    if (request_id) *request_id = next_request_id;
    return true;
}

bool pik_control_take_ack(uint32_t *request_id, pik_control_ack_status_t *status,
                          const uint8_t **payload, size_t *payload_len) {
    if (!ack_available) return false;
    ack_available = false;
    if (request_id) *request_id = ack_request;
    if (status) *status = ack_status;
    if (payload) *payload = ack_payload;
    if (payload_len) *payload_len = ack_payload_len;
    return true;
}

const char *pik_control_action_name(pik_control_action_t action) {
    switch (action) {
    case PIK_CONTROL_ACTION_RESTART_PIK1: return "restart-pik1";
    case PIK_CONTROL_ACTION_REBOOT: return "reboot";
    case PIK_CONTROL_ACTION_POWEROFF: return "poweroff";
    case PIK_CONTROL_ACTION_STATUS: return "status";
    case PIK_CONTROL_ACTION_RESTART_WIFI: return "restart-wifi";
    case PIK_CONTROL_ACTION_RESTART_KLIPPER: return "restart-klipper";
    default: return "unknown";
    }
}

const char *pik_control_ack_status_name(pik_control_ack_status_t status) {
    switch (status) {
    case PIK_CONTROL_ACK_OK: return "ok";
    case PIK_CONTROL_ACK_UNKNOWN_ACTION: return "unknown-action";
    case PIK_CONTROL_ACK_INTERNAL_ERROR: return "internal-error";
    default: return "bad-status";
    }
}

static void reset_state(void) {
    now_ms = 100000;
    send_ack_ok = true;
    send_ack_calls = 0;
    send_ack_request = 0;
    send_ack_status = 0;
    send_ack_payload_len = 0;
    send_command_ok = true;
    send_command_calls = 0;
    sent_action = 0;
    next_request_id = 1000;
    ack_available = false;
    ack_request = 0;
    ack_status = 0;
    ack_payload = NULL;
    ack_payload_len = 0;
    pik_commands_init("test", true);
}

static int connect_control_socket(const char *path) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", path);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static bool dispatch_local_event(int epfd) {
    struct epoll_event ev;
    int n;
    do {
        n = epoll_wait(epfd, &ev, 1, 1000);
    } while (n < 0 && errno == EINTR);
    if (n <= 0) return false;
    if (pik_commands_owns_event(ev.data.ptr)) {
        pik_commands_dispatch(ev.data.ptr, now_ms);
        return true;
    }
    return false;
}

static bool read_reply(int fd, char *buf, size_t cap) {
    size_t len = 0;
    while (len < cap - 1) {
        ssize_t n = read(fd, buf + len, cap - 1 - len);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) return false;
        if (n == 0) break;
        len += (size_t)n;
    }
    buf[len] = '\0';
    return len > 0;
}

static void test_parse_control_actions(void) {
    pik_control_action_t action = 0;
    reset_state();
    CHECK(pik_commands_parse_action("status", &action));
    CHECK(action == PIK_CONTROL_ACTION_STATUS);
    CHECK(pik_commands_parse_action("restart-pik1", &action));
    CHECK(action == PIK_CONTROL_ACTION_RESTART_PIK1);
    CHECK(pik_commands_parse_action("restart-wifi", &action));
    CHECK(action == PIK_CONTROL_ACTION_RESTART_WIFI);
    CHECK(pik_commands_parse_action("restart-klipper", &action));
    CHECK(action == PIK_CONTROL_ACTION_RESTART_KLIPPER);
    CHECK(!pik_commands_parse_action("bogus", &action));
}

static void test_status_ack_payload(void) {
    reset_state();
    pik_commands_set_service_flags(PIK_CONTROL_SERVICE_SERIAL);
    pik_commands_on_command(PIK_CONTROL_ACTION_STATUS, 77);

    CHECK(send_ack_calls == 1);
    CHECK(send_ack_request == 77);
    CHECK(send_ack_status == PIK_CONTROL_ACK_OK);
    send_ack_payload[send_ack_payload_len] = '\0';
    CHECK(strstr((char *)send_ack_payload, "side=test") != NULL);
    CHECK(strstr((char *)send_ack_payload, "services=serial") != NULL);
}

static void test_remote_action_requires_ack_success(void) {
    pik_control_action_t action = 0;
    reset_state();

    pik_commands_on_command(PIK_CONTROL_ACTION_REBOOT, 88);

    CHECK(send_ack_calls == 1);
    CHECK(send_ack_status == PIK_CONTROL_ACK_OK);
    CHECK(!pik_commands_action_due(now_ms, &action));
    CHECK(pik_commands_action_due(now_ms + PIK_REMOTE_ACTION_DELAY_MS, &action));
    CHECK(action == PIK_CONTROL_ACTION_REBOOT);
    CHECK(!pik_commands_action_due(
        now_ms + PIK_REMOTE_ACTION_DELAY_MS, &action));
}

static void test_restart_wifi_is_scheduled(void) {
    pik_control_action_t action = 0;
    reset_state();

    pik_commands_on_command(PIK_CONTROL_ACTION_RESTART_WIFI, 89);

    CHECK(send_ack_calls == 1);
    CHECK(send_ack_status == PIK_CONTROL_ACK_OK);
    CHECK(pik_commands_action_due(
        now_ms + PIK_REMOTE_ACTION_DELAY_MS, &action));
    CHECK(action == PIK_CONTROL_ACTION_RESTART_WIFI);
}

static void test_restart_klipper_direction(void) {
    pik_control_action_t action = 0;
    reset_state();

    pik_commands_on_command(PIK_CONTROL_ACTION_RESTART_KLIPPER, 90);
    CHECK(send_ack_calls == 1);
    CHECK(send_ack_status == PIK_CONTROL_ACK_OK);
    CHECK(pik_commands_action_due(
        now_ms + PIK_REMOTE_ACTION_DELAY_MS, &action));
    CHECK(action == PIK_CONTROL_ACTION_RESTART_KLIPPER);

    reset_state();
    pik_commands_init("test", false);
    pik_commands_on_command(PIK_CONTROL_ACTION_RESTART_KLIPPER, 91);
    CHECK(send_ack_calls == 1);
    CHECK(send_ack_status == PIK_CONTROL_ACK_OK);
    CHECK(!pik_commands_action_due(
        now_ms + PIK_REMOTE_ACTION_DELAY_MS, &action));
}

static void test_remote_action_not_scheduled_on_ack_failure(void) {
    pik_control_action_t action = 0;
    reset_state();
    send_ack_ok = false;

    pik_commands_on_command(PIK_CONTROL_ACTION_POWEROFF, 99);

    CHECK(send_ack_calls == 1);
    CHECK(!pik_commands_action_due(now_ms + PIK_REMOTE_ACTION_DELAY_MS, &action));
}

static void test_remote_action_cannot_be_overwritten(void) {
    pik_control_action_t action = 0;
    reset_state();

    pik_commands_on_command(PIK_CONTROL_ACTION_REBOOT, 1);
    pik_commands_on_command(PIK_CONTROL_ACTION_POWEROFF, 2);

    CHECK(send_ack_calls == 2);
    CHECK(send_ack_request == 2);
    CHECK(send_ack_status == PIK_CONTROL_ACK_INTERNAL_ERROR);
    CHECK(pik_commands_action_due(
        now_ms + PIK_REMOTE_ACTION_DELAY_MS, &action));
    CHECK(action == PIK_CONTROL_ACTION_REBOOT);
}

static void test_signal_restart_pik1_ack(void) {
    reset_state();

    pik_commands_request_restart_pik1(true, now_ms);
    CHECK(send_command_calls == 1);
    CHECK(sent_action == PIK_CONTROL_ACTION_RESTART_PIK1);
    CHECK(pik_commands_signal_pending());
    CHECK(!pik_commands_signal_done());

    ack_available = true;
    ack_request = next_request_id;
    ack_status = PIK_CONTROL_ACK_OK;
    pik_commands_check_acks(now_ms);
    CHECK(!pik_commands_signal_pending());
    CHECK(pik_commands_signal_done());
}

static void test_signal_restart_pik1_timeout(void) {
    reset_state();

    pik_commands_request_restart_pik1(true, now_ms);
    pik_commands_check_acks(now_ms + PIK_COMMAND_ACK_TIMEOUT_MS);
    CHECK(!pik_commands_signal_pending());
    CHECK(pik_commands_signal_done());
}

static void test_local_command_roundtrip(void) {
    char reply[256];
    reset_state();

    int epfd = epoll_create1(EPOLL_CLOEXEC);
    CHECK(epfd >= 0);
    CHECK(pik_commands_start(epfd));

    int peer = connect_control_socket(pik_commands_sock_path());
    CHECK(peer >= 0);
    CHECK(dispatch_local_event(epfd));

    CHECK(test_write_all(peer, "status\n", sizeof("status\n") - 1u));
    CHECK(dispatch_local_event(epfd));
    CHECK(send_command_calls == 1);
    CHECK(sent_action == PIK_CONTROL_ACTION_STATUS);

    ack_available = true;
    ack_request = next_request_id;
    ack_status = PIK_CONTROL_ACK_OK;
    ack_payload = (const uint8_t *)"side=mcu services=serial,tunnel";
    ack_payload_len = strlen((const char *)ack_payload);
    pik_commands_check_acks(now_ms);
    CHECK(read_reply(peer, reply, sizeof(reply)));
    CHECK(strcmp(reply, "OK side=mcu services=serial,tunnel\n") == 0);

    close(peer);
    pik_commands_cleanup();
    close(epfd);
}

static void test_fragmented_local_command(void) {
    char reply[64];
    reset_state();

    int epfd = epoll_create1(EPOLL_CLOEXEC);
    CHECK(epfd >= 0);
    CHECK(pik_commands_start(epfd));

    int peer = connect_control_socket(pik_commands_sock_path());
    CHECK(peer >= 0);
    CHECK(dispatch_local_event(epfd));

    CHECK(test_write_all(peer, "reb", sizeof("reb") - 1u));
    CHECK(dispatch_local_event(epfd));
    CHECK(send_command_calls == 0);

    CHECK(test_write_all(peer, "oot\n", sizeof("oot\n") - 1u));
    CHECK(dispatch_local_event(epfd));
    CHECK(send_command_calls == 1);
    CHECK(sent_action == PIK_CONTROL_ACTION_REBOOT);

    ack_available = true;
    ack_request = next_request_id;
    ack_status = PIK_CONTROL_ACK_OK;
    pik_commands_check_acks(now_ms);
    CHECK(read_reply(peer, reply, sizeof(reply)));
    CHECK(strcmp(reply, "OK\n") == 0);

    close(peer);
    pik_commands_cleanup();
    close(epfd);
}

static void test_local_error_ack_includes_reason(void) {
    char reply[128];
    reset_state();

    int epfd = epoll_create1(EPOLL_CLOEXEC);
    CHECK(epfd >= 0);
    CHECK(pik_commands_start(epfd));

    int peer = connect_control_socket(pik_commands_sock_path());
    CHECK(peer >= 0);
    CHECK(dispatch_local_event(epfd));

    CHECK(test_write_all(peer, "reboot\n", sizeof("reboot\n") - 1u));
    CHECK(dispatch_local_event(epfd));

    ack_available = true;
    ack_request = next_request_id;
    ack_status = PIK_CONTROL_ACK_INTERNAL_ERROR;
    ack_payload = (const uint8_t *)"exec failed";
    ack_payload_len = strlen((const char *)ack_payload);
    pik_commands_check_acks(now_ms);
    CHECK(read_reply(peer, reply, sizeof(reply)));
    CHECK(strcmp(reply, "ERR peer internal-error exec failed\n") == 0);

    close(peer);
    pik_commands_cleanup();
    close(epfd);
}

static void test_local_ack_timeout(void) {
    char reply[128];
    reset_state();

    int epfd = epoll_create1(EPOLL_CLOEXEC);
    CHECK(epfd >= 0);
    CHECK(pik_commands_start(epfd));

    int peer = connect_control_socket(pik_commands_sock_path());
    CHECK(peer >= 0);
    CHECK(dispatch_local_event(epfd));

    CHECK(test_write_all(peer, "status\n", sizeof("status\n") - 1u));
    CHECK(dispatch_local_event(epfd));
    pik_commands_check_acks(now_ms + PIK_COMMAND_ACK_TIMEOUT_MS);
    CHECK(read_reply(peer, reply, sizeof(reply)));
    CHECK(strcmp(reply, "ERR peer ack timeout\n") == 0);

    close(peer);
    pik_commands_cleanup();
    close(epfd);
}

static void test_command_without_ready_peer_fails(void) {
    char reply[128];
    reset_state();
    send_command_ok = false;

    int epfd = epoll_create1(EPOLL_CLOEXEC);
    CHECK(epfd >= 0);
    CHECK(pik_commands_start(epfd));
    int peer = connect_control_socket(pik_commands_sock_path());
    CHECK(peer >= 0);
    CHECK(dispatch_local_event(epfd));

    CHECK(test_write_all(peer, "status\n", sizeof("status\n") - 1u));
    CHECK(dispatch_local_event(epfd));
    CHECK(read_reply(peer, reply, sizeof(reply)));
    CHECK(strcmp(reply, "ERR peer not ready\n") == 0);

    close(peer);
    pik_commands_cleanup();
    close(epfd);
}

int main(void) {
    const char *sock = getenv(PIK_CONTROL_SOCK_ENV);
    CHECK(sock && *sock);
    unlink(sock);

    test_parse_control_actions();
    test_status_ack_payload();
    test_remote_action_requires_ack_success();
    test_restart_wifi_is_scheduled();
    test_restart_klipper_direction();
    test_remote_action_not_scheduled_on_ack_failure();
    test_remote_action_cannot_be_overwritten();
    test_signal_restart_pik1_ack();
    test_signal_restart_pik1_timeout();
    test_local_command_roundtrip();
    test_fragmented_local_command();
    test_local_error_ack_includes_reason();
    test_local_ack_timeout();
    test_command_without_ready_peer_fails();

    unlink(sock);
    if (failures) {
        fprintf(stderr, "test_commands: %d failure(s)\n", failures);
        return 1;
    }
    puts("test_commands: ok");
    return 0;
}
