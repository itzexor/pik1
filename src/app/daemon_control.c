#include "daemon_control.h"

#include "local_control.h"
#include "logging.h"
#include "pik_proto.h"
#include "product.h"
#include "util.h"

#include <limits.h>
#include <stdio.h>

#define LOG(...) pik_log("pik1", __VA_ARGS__)

typedef struct {
    const char *side_name;
    uint32_t service_flags;
    pik_control_action_t remote_action;
    bool remote_action_pending;
    int64_t remote_action_at_ms;
    bool signal_command_pending;
    bool signal_command_done;
    uint32_t signal_request_id;
    int64_t signal_deadline_ms;
} daemon_control_state_t;

static daemon_control_state_t g_ctl;

static void append_service_names(char *buf, size_t cap, uint32_t flags) {
    uint32_t known = PIK_CONTROL_SERVICE_SERIAL | PIK_CONTROL_SERVICE_TUNNEL;
    if ((flags & known) == known)
        snprintf(buf, cap, "serial,tunnel");
    else if (flags & PIK_CONTROL_SERVICE_SERIAL)
        snprintf(buf, cap, "serial");
    else if (flags & PIK_CONTROL_SERVICE_TUNNEL)
        snprintf(buf, cap, "tunnel");
    else
        snprintf(buf, cap, "none");
}

void pik_daemon_control_init(const char *side_name) {
    g_ctl.side_name = side_name;
    g_ctl.service_flags = 0;
    g_ctl.remote_action = 0;
    g_ctl.remote_action_pending = false;
    g_ctl.remote_action_at_ms = 0;
    g_ctl.signal_command_pending = false;
    g_ctl.signal_command_done = false;
    g_ctl.signal_request_id = 0;
    g_ctl.signal_deadline_ms = 0;
}

void pik_daemon_set_service_flags(uint32_t flags) {
    if (g_ctl.service_flags == flags) return;
    g_ctl.service_flags = flags;
    if (pik_control_ready() &&
        !pik_control_send_service_state(g_ctl.service_flags))
        LOG("failed to send service state update");
}

void pik_daemon_set_service_flag(uint32_t flag, bool up) {
    uint32_t flags = up ? (g_ctl.service_flags | flag)
                        : (g_ctl.service_flags & ~flag);
    pik_daemon_set_service_flags(flags);
}

void pik_daemon_on_control_command(pik_control_action_t action, uint32_t request_id) {
    LOG("received command %s request=%u", pik_control_action_name(action), request_id);
    if (action == PIK_CONTROL_ACTION_STATUS) {
        char services[24];
        char peer_services[24];
        uint32_t peer_flags;
        append_service_names(services, sizeof(services), g_ctl.service_flags);
        if (pik_control_peer_service_state(&peer_flags))
            append_service_names(peer_services, sizeof(peer_services), peer_flags);
        else
            snprintf(peer_services, sizeof(peer_services), "unknown");

        char status[PIK_CTRL_ACK_MAX_PAYLOAD + 1u];
        int n = snprintf(status, sizeof(status),
                         "side=%s release=%s protocol=%u services=%s peer_services=%s",
                         g_ctl.side_name, PIK1_RELEASE_VERSION,
                         PIK1_PROTOCOL_VERSION, services, peer_services);
        if (n < 0 || (size_t)n >= sizeof(status)) {
            if (!pik_control_send_ack(request_id,
                                      PIK_CONTROL_ACK_INTERNAL_ERROR, NULL, 0))
                LOG("failed to send status error ACK request=%u", request_id);
            return;
        }
        if (!pik_control_send_ack(request_id, PIK_CONTROL_ACK_OK,
                                  (const uint8_t *)status, (size_t)n))
            LOG("failed to send status ACK request=%u", request_id);
        return;
    }

    if (g_ctl.remote_action_pending) {
        static const uint8_t busy[] = "action already pending";
        if (!pik_control_send_ack(request_id, PIK_CONTROL_ACK_INTERNAL_ERROR,
                                  busy, sizeof(busy) - 1u))
            LOG("failed to send busy ACK request=%u", request_id);
        return;
    }

    if (!pik_control_send_ack(request_id, PIK_CONTROL_ACK_OK, NULL, 0)) {
        LOG("failed to send command ACK request=%u", request_id);
        return;
    }
    g_ctl.remote_action = action;
    g_ctl.remote_action_pending = true;
    g_ctl.remote_action_at_ms = pik_now_ms() + PIK_REMOTE_ACTION_DELAY_MS;
}

void pik_daemon_check_acks(int64_t now) {
    uint32_t request_id;
    pik_control_ack_status_t status;
    const uint8_t *payload;
    size_t payload_len;
    while (pik_control_take_ack(&request_id, &status, &payload, &payload_len)) {
        if (pik_local_control_pending() &&
            request_id == pik_local_control_request_id()) {
            pik_local_control_complete(status, payload, payload_len);
        } else if (g_ctl.signal_command_pending &&
                   request_id == g_ctl.signal_request_id) {
            LOG("restart command ack status=%s", pik_control_ack_status_name(status));
            g_ctl.signal_command_pending = false;
            g_ctl.signal_command_done = true;
        } else {
            LOG("command ack request=%u status=%s",
                request_id, pik_control_ack_status_name(status));
        }
    }
    pik_local_control_check_timeout(now);
    if (g_ctl.signal_command_pending && now >= g_ctl.signal_deadline_ms) {
        LOG("restart command ack timeout");
        g_ctl.signal_command_pending = false;
        g_ctl.signal_command_done = true;
    }
}

int64_t pik_daemon_control_deadline(void) {
    int64_t dl = pik_local_control_deadline();
    if (g_ctl.signal_command_pending && g_ctl.signal_deadline_ms < dl)
        dl = g_ctl.signal_deadline_ms;
    return dl;
}

bool pik_daemon_signal_pending(void) {
    return g_ctl.signal_command_pending;
}

bool pik_daemon_signal_done(void) {
    return g_ctl.signal_command_done;
}

void pik_daemon_request_restart_peer(bool can_signal_peer_restart, int64_t now) {
    if (pik_local_control_pending()) {
        LOG("ignoring SIGUSR1 while a local command is pending");
        g_ctl.signal_command_done = true;
    } else if (can_signal_peer_restart && !g_ctl.signal_command_pending &&
               !g_ctl.signal_command_done &&
               pik_control_send_command(PIK_CONTROL_ACTION_RESTART_PEER,
                                        &g_ctl.signal_request_id)) {
        g_ctl.signal_command_pending = true;
        g_ctl.signal_deadline_ms = now + PIK_COMMAND_ACK_TIMEOUT_MS;
    } else {
        g_ctl.signal_command_done = true;
    }
}

bool pik_daemon_remote_action_due(int64_t now, pik_control_action_t *action) {
    if (!g_ctl.remote_action_pending || now < g_ctl.remote_action_at_ms)
        return false;
    if (action) *action = g_ctl.remote_action;
    g_ctl.remote_action = 0;
    g_ctl.remote_action_pending = false;
    g_ctl.remote_action_at_ms = 0;
    return true;
}
