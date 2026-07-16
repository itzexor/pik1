#include "daemon_control.h"
#include "local_control.h"
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
static uint8_t send_ack_payload[256];
static size_t send_ack_payload_len;

static bool peer_state_known;
static uint32_t peer_state_flags;

static bool send_command_ok = true;
static int send_command_calls;
static pik_control_action_t sent_action;
static uint32_t next_request_id = 1000;

static bool ack_available;
static uint32_t ack_request;
static pik_control_ack_status_t ack_status;
static const char *ack_payload;

int64_t pik_now_ms(void) {
    return now_ms;
}

bool pik_control_ready(void) {
    return false;
}

bool pik_control_send_link_state(uint32_t flags) {
    (void)flags;
    return true;
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

bool pik_control_peer_link_state(uint32_t *flags) {
    if (!peer_state_known) return false;
    if (flags) *flags = peer_state_flags;
    return true;
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
    if (payload) *payload = (const uint8_t *)ack_payload;
    if (payload_len) *payload_len = ack_payload ? strlen(ack_payload) : 0;
    return true;
}

const char *pik_control_action_name(pik_control_action_t action) {
    switch (action) {
    case PIK_CONTROL_ACTION_RESTART_PEER: return "restart-peer";
    case PIK_CONTROL_ACTION_REBOOT_PEER: return "reboot-peer";
    case PIK_CONTROL_ACTION_POWEROFF_PEER: return "poweroff-peer";
    case PIK_CONTROL_ACTION_STATUS: return "status";
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
    peer_state_known = false;
    peer_state_flags = 0;
    send_command_ok = true;
    send_command_calls = 0;
    sent_action = 0;
    next_request_id = 1000;
    ack_available = false;
    ack_request = 0;
    ack_status = 0;
    ack_payload = NULL;
    pik_daemon_control_init("test");
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
    if (pik_local_control_owns_listen(ev.data.ptr)) {
        pik_local_control_accept();
        return true;
    }
    if (pik_local_control_owns_client(ev.data.ptr)) {
        pik_local_control_read(now_ms, pik_daemon_signal_pending());
        return true;
    }
    return false;
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

static void test_parse_control_actions(void) {
    pik_control_action_t action = 0;
    reset_state();
    CHECK(pik_parse_control_action("status-peer", &action));
    CHECK(action == PIK_CONTROL_ACTION_STATUS);
    CHECK(pik_parse_control_action("restart-peer", &action));
    CHECK(action == PIK_CONTROL_ACTION_RESTART_PEER);
    CHECK(!pik_parse_control_action("bogus", &action));
}

static void test_status_ack_payload(void) {
    reset_state();
    pik_daemon_set_link_flags(PIK_CONTROL_LINK_SERIAL);
    peer_state_known = true;
    peer_state_flags = PIK_CONTROL_LINK_TCP;

    pik_daemon_on_control_command(PIK_CONTROL_ACTION_STATUS, 77);

    CHECK(send_ack_calls == 1);
    CHECK(send_ack_request == 77);
    CHECK(send_ack_status == PIK_CONTROL_ACK_OK);
    CHECK(send_ack_payload_len > 0);
    CHECK(memmem(send_ack_payload, send_ack_payload_len, "links=serial", 12) != NULL);
    CHECK(memmem(send_ack_payload, send_ack_payload_len, "peer=tcp", 8) != NULL);
}

static void test_remote_action_requires_ack_success(void) {
    pik_control_action_t action = 0;
    reset_state();

    pik_daemon_on_control_command(PIK_CONTROL_ACTION_REBOOT_PEER, 88);

    CHECK(send_ack_calls == 1);
    CHECK(send_ack_status == PIK_CONTROL_ACK_OK);
    CHECK(!pik_daemon_remote_action_due(now_ms, &action));
    CHECK(pik_daemon_remote_action_due(now_ms + PIK_REMOTE_ACTION_DELAY_MS, &action));
    CHECK(action == PIK_CONTROL_ACTION_REBOOT_PEER);
}

static void test_remote_action_not_scheduled_on_ack_failure(void) {
    pik_control_action_t action = 0;
    reset_state();
    send_ack_ok = false;

    pik_daemon_on_control_command(PIK_CONTROL_ACTION_POWEROFF_PEER, 99);

    CHECK(send_ack_calls == 1);
    CHECK(!pik_daemon_remote_action_due(now_ms + PIK_REMOTE_ACTION_DELAY_MS, &action));
}

static void test_signal_restart_peer_ack(void) {
    reset_state();

    pik_daemon_request_restart_peer(true, now_ms);
    CHECK(send_command_calls == 1);
    CHECK(sent_action == PIK_CONTROL_ACTION_RESTART_PEER);
    CHECK(pik_daemon_signal_pending());
    CHECK(!pik_daemon_signal_done());

    ack_available = true;
    ack_request = next_request_id;
    ack_status = PIK_CONTROL_ACK_OK;
    pik_daemon_check_acks(now_ms);
    CHECK(!pik_daemon_signal_pending());
    CHECK(pik_daemon_signal_done());
}

static void test_signal_restart_peer_timeout(void) {
    reset_state();

    pik_daemon_request_restart_peer(true, now_ms);
    pik_daemon_check_acks(now_ms + PIK_COMMAND_ACK_TIMEOUT_MS);
    CHECK(!pik_daemon_signal_pending());
    CHECK(pik_daemon_signal_done());
}

static void test_local_command_roundtrip(void) {
    char reply[128];
    reset_state();

    int epfd = epoll_create1(EPOLL_CLOEXEC);
    CHECK(epfd >= 0);
    CHECK(pik_local_control_start(epfd));

    int peer = connect_control_socket(pik_local_control_sock_path());
    CHECK(peer >= 0);
    CHECK(dispatch_local_event(epfd));

    CHECK(test_write_all(peer, "status-peer\n", 12));
    CHECK(dispatch_local_event(epfd));
    CHECK(send_command_calls == 1);
    CHECK(sent_action == PIK_CONTROL_ACTION_STATUS);
    CHECK(pik_local_control_pending());
    CHECK(pik_local_control_deadline() == now_ms + PIK_COMMAND_ACK_TIMEOUT_MS);

    ack_available = true;
    ack_request = next_request_id;
    ack_status = PIK_CONTROL_ACK_OK;
    ack_payload = "uart=pty links=serial";
    pik_daemon_check_acks(now_ms);
    CHECK(read_reply(peer, reply, sizeof(reply)));
    CHECK(strcmp(reply, "OK uart=pty links=serial\n") == 0);
    CHECK(!pik_local_control_pending());

    close(peer);
    pik_local_control_cleanup();
    close(epfd);
}

static void test_local_error_ack_includes_reason(void) {
    char reply[128];
    reset_state();

    int epfd = epoll_create1(EPOLL_CLOEXEC);
    CHECK(epfd >= 0);
    CHECK(pik_local_control_start(epfd));

    int peer = connect_control_socket(pik_local_control_sock_path());
    CHECK(peer >= 0);
    CHECK(dispatch_local_event(epfd));

    CHECK(test_write_all(peer, "reboot-peer\n", 12));
    CHECK(dispatch_local_event(epfd));

    ack_available = true;
    ack_request = next_request_id;
    ack_status = PIK_CONTROL_ACK_INTERNAL_ERROR;
    ack_payload = "exec failed";
    pik_daemon_check_acks(now_ms);
    CHECK(read_reply(peer, reply, sizeof(reply)));
    CHECK(strcmp(reply, "ERR peer internal-error exec failed\n") == 0);

    close(peer);
    pik_local_control_cleanup();
    close(epfd);
}

static void test_local_ack_timeout(void) {
    char reply[64];
    reset_state();

    int epfd = epoll_create1(EPOLL_CLOEXEC);
    CHECK(epfd >= 0);
    CHECK(pik_local_control_start(epfd));

    int peer = connect_control_socket(pik_local_control_sock_path());
    CHECK(peer >= 0);
    CHECK(dispatch_local_event(epfd));

    CHECK(test_write_all(peer, "status-peer\n", 12));
    CHECK(dispatch_local_event(epfd));
    pik_daemon_check_acks(now_ms + PIK_COMMAND_ACK_TIMEOUT_MS);
    CHECK(read_reply(peer, reply, sizeof(reply)));
    CHECK(strcmp(reply, "ERR peer ack timeout\n") == 0);
    CHECK(!pik_local_control_pending());

    close(peer);
    pik_local_control_cleanup();
    close(epfd);
}

int main(void) {
    const char *sock = getenv(PIK_CONTROL_SOCK_ENV);
    CHECK(sock && *sock);
    unlink(sock);

    test_parse_control_actions();
    test_status_ack_payload();
    test_remote_action_requires_ack_success();
    test_remote_action_not_scheduled_on_ack_failure();
    test_signal_restart_peer_ack();
    test_signal_restart_peer_timeout();
    test_local_command_roundtrip();
    test_local_error_ack_includes_reason();
    test_local_ack_timeout();

    unlink(sock);
    if (failures) {
        fprintf(stderr, "test_app_control: %d failure(s)\n", failures);
        return 1;
    }
    puts("test_app_control: ok");
    return 0;
}
