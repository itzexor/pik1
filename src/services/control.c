// Control service: handshake, liveness, and peer commands on channel 0

#include "control.h"
#include "pik_proto.h"
#include "session.h"
#include "logging.h"
#include "product.h"
#include "util.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define HELLO_RETRY_MS 2000
#define PING_IDLE_MS  3000
#define LINK_DEAD_MS  10000

/* While the peer stays unreachable the handshake fails every LINK_DEAD_MS;
 * log the first failure, then one summary line per this interval. */
#define HANDSHAKE_REPORT_MS 600000

typedef struct {
    pik_control_role_t role;
    pik_control_command_fn on_command;
    pik_control_ready_fn on_ready;
    bool ready;

    uint32_t next_request_id;
    bool ack_pending;
    uint32_t ack_request_id;
    pik_control_ack_status_t ack_status;
    uint8_t ack_payload[PIK_CTRL_ACK_MAX_PAYLOAD];
    size_t ack_payload_len;

    uint8_t local_channels[PIK_CTRL_MAX_PAYLOAD - 2u];
    size_t local_channel_count;
    pik_control_tcp_role_t local_tcp_role;

    uint32_t handshake_fails;          /* consecutive handshake RX timeouts */
    uint32_t handshake_fails_reported;
    int64_t  handshake_report_ms;
} control_t;

static control_t g_ctrl;

#define LOG(...) pik_log("ctrl", __VA_ARGS__)

const char *pik_control_action_name(pik_control_action_t action) {
    switch (action) {
    case PIK_CONTROL_ACTION_RESTART_PEER: return "restart-peer";
    case PIK_CONTROL_ACTION_REBOOT_PEER: return "reboot-peer";
    case PIK_CONTROL_ACTION_POWEROFF_PEER: return "poweroff-peer";
    case PIK_CONTROL_ACTION_STATUS: return "status";
    case PIK_CONTROL_ACTION_WIFI_RESET_PEER: return "wifi-reset-peer";
    default: return "unknown";
    }
}

const char *pik_control_ack_status_name(pik_control_ack_status_t status) {
    switch (status) {
    case PIK_CONTROL_ACK_OK: return "ok";
    case PIK_CONTROL_ACK_UNKNOWN_ACTION: return "unknown-action";
    case PIK_CONTROL_ACK_INTERNAL_ERROR: return "internal-error";
    default: return "unknown-status";
    }
}

static bool action_valid(pik_control_action_t action) {
    switch (action) {
    case PIK_CONTROL_ACTION_RESTART_PEER:
    case PIK_CONTROL_ACTION_REBOOT_PEER:
    case PIK_CONTROL_ACTION_POWEROFF_PEER:
    case PIK_CONTROL_ACTION_STATUS:
    case PIK_CONTROL_ACTION_WIFI_RESET_PEER:
        return true;
    default:
        return false;
    }
}

static bool ack_status_valid(pik_control_ack_status_t status) {
    switch (status) {
    case PIK_CONTROL_ACK_OK:
    case PIK_CONTROL_ACK_UNKNOWN_ACTION:
    case PIK_CONTROL_ACK_INTERNAL_ERROR:
        return true;
    default:
        return false;
    }
}

static void clear_pending_ack(void) {
    g_ctrl.ack_pending = false;
    g_ctrl.ack_request_id = 0;
    g_ctrl.ack_status = 0;
    g_ctrl.ack_payload_len = 0;
}

static bool enqueue_frame(uint8_t type, const uint8_t *payload, size_t plen) {
    return pik_session_enqueue(PIK_SESSION_CLASS_CONTROL, type,
                               PIK_CH_CONTROL, payload, plen);
}

static bool send_hello(void) {
    uint8_t p[PIK_CTRL_HELLO_LEN];
    memset(p, 0, sizeof(p));
    p[0] = PIK_CTRL_HELLO_MAGIC_0;
    p[1] = PIK_CTRL_HELLO_MAGIC_1;
    p[2] = PIK_CTRL_HELLO_MAGIC_2;
    p[3] = PIK_CTRL_HELLO_MAGIC_3;
    pik_put_u32le(p + 4, PIK1_PROTOCOL_VERSION);
    p[8] = (uint8_t)g_ctrl.role;
    snprintf((char *)p + 9, PIK_CTRL_RELEASE_LEN, "%s", PIK1_RELEASE_VERSION);
    return enqueue_frame(PIK_FRAME_CTRL_HELLO, p, sizeof(p));
}

static const char *tcp_role_name(pik_control_tcp_role_t role) {
    switch (role) {
    case PIK_CONTROL_TCP_NONE: return "none";
    case PIK_CONTROL_TCP_LISTEN: return "listen";
    case PIK_CONTROL_TCP_FORWARD: return "forward";
    default: return "unknown";
    }
}

static bool send_config(void) {
    uint8_t p[PIK_CTRL_MAX_PAYLOAD];
    if (g_ctrl.local_channel_count > sizeof(p) - 2)
        return false;
    p[0] = (uint8_t)g_ctrl.local_tcp_role;
    p[1] = (uint8_t)g_ctrl.local_channel_count;
    if (g_ctrl.local_channel_count)
        memcpy(p + 2, g_ctrl.local_channels, g_ctrl.local_channel_count);
    return enqueue_frame(PIK_FRAME_CTRL_CONFIG, p,
                         g_ctrl.local_channel_count + 2);
}

static bool local_channel_present(uint8_t id) {
    for (size_t i = 0; i < g_ctrl.local_channel_count; i++)
        if (g_ctrl.local_channels[i] == id)
            return true;
    return false;
}

static bool handle_config(const uint8_t *p, size_t len) {
    if (len < 2 || p[1] != len - 2) {
        LOG("bad CONFIG");
        return false;
    }

    pik_control_tcp_role_t peer_tcp = (pik_control_tcp_role_t)p[0];
    if (peer_tcp > PIK_CONTROL_TCP_FORWARD) {
        LOG("bad CONFIG tcp role=%u", p[0]);
        return false;
    }

    bool peer_present[UINT8_MAX + 1] = { false };
    for (size_t i = 2; i < len; i++) {
        uint8_t id = p[i];
        if (!pik_mux_cli_valid(id) || peer_present[id]) {
            LOG("bad CONFIG channel id=%u", id);
            return false;
        }
        peer_present[id] = true;
        if (!local_channel_present(id))
            LOG("warning: channel id %u is not configured on this side", id);
    }

    for (size_t i = 0; i < g_ctrl.local_channel_count; i++) {
        uint8_t id = g_ctrl.local_channels[i];
        if (!peer_present[id])
            LOG("warning: channel id %u is not configured on peer", id);
    }

    if (g_ctrl.local_tcp_role == PIK_CONTROL_TCP_NONE &&
        peer_tcp != PIK_CONTROL_TCP_NONE) {
        LOG("warning: TCP tunnel is configured on peer as %s but not on this side",
            tcp_role_name(peer_tcp));
    } else if (g_ctrl.local_tcp_role != PIK_CONTROL_TCP_NONE &&
               peer_tcp == PIK_CONTROL_TCP_NONE) {
        LOG("warning: TCP tunnel is configured on this side as %s but not on peer",
            tcp_role_name(g_ctrl.local_tcp_role));
    } else if (g_ctrl.local_tcp_role != PIK_CONTROL_TCP_NONE &&
               g_ctrl.local_tcp_role == peer_tcp) {
        LOG("warning: TCP tunnel is configured as %s on both sides",
            tcp_role_name(g_ctrl.local_tcp_role));
    }
    return true;
}

static bool handle_hello(const uint8_t *p, size_t len) {
    if (len != PIK_CTRL_HELLO_LEN ||
        p[0] != PIK_CTRL_HELLO_MAGIC_0 || p[1] != PIK_CTRL_HELLO_MAGIC_1 ||
        p[2] != PIK_CTRL_HELLO_MAGIC_2 || p[3] != PIK_CTRL_HELLO_MAGIC_3) {
        LOG("bad HELLO");
        return false;
    }

    uint32_t proto = pik_get_u32le(p + 4);
    uint8_t role = p[8];
    char release[PIK_CTRL_RELEASE_LEN + 1];
    memcpy(release, p + 9, PIK_CTRL_RELEASE_LEN);
    release[PIK_CTRL_RELEASE_LEN] = '\0';

    if (proto != PIK1_PROTOCOL_VERSION) {
        LOG("protocol mismatch: local=%u remote=%u remote_release=%s",
            PIK1_PROTOCOL_VERSION, proto, release);
        return false;
    }
    uint8_t expected_role = g_ctrl.role == PIK_CONTROL_ROLE_PTY
        ? PIK_CONTROL_ROLE_MCU : PIK_CONTROL_ROLE_PTY;
    if (role != expected_role) {
        LOG("peer role mismatch: expected=%u got=%u", expected_role, role);
        return false;
    }
    if (strcmp(release, PIK1_RELEASE_VERSION) != 0)
        LOG("release mismatch: local=%s remote=%s", PIK1_RELEASE_VERSION, release);

    if (!g_ctrl.ready) {
        g_ctrl.ready = true;
        if (g_ctrl.handshake_fails) {
            LOG("handshake recovered after %u failed attempts", g_ctrl.handshake_fails);
            g_ctrl.handshake_fails = 0;
            g_ctrl.handshake_fails_reported = 0;
        }
        LOG("link up: release=%s protocol=%u", release, proto);
        if (!send_hello())
            return false;
        if (!send_config())
            return false;
        if (g_ctrl.on_ready)
            g_ctrl.on_ready();
        if (pik_session_link()->failed)
            return false;
    }
    return true;
}

bool pik_control_on_frame(uint8_t type, const uint8_t *p, size_t len) {
    switch (type) {
    case PIK_FRAME_CTRL_HELLO:
        return handle_hello(p, len);
    case PIK_FRAME_CTRL_PING:
        if (len != 0) {
            LOG("bad PING len=%zu", len);
            return false;
        }
        return enqueue_frame(PIK_FRAME_CTRL_PONG, NULL, 0);
    case PIK_FRAME_CTRL_PONG:
        if (len != 0) {
            LOG("bad PONG len=%zu", len);
            return false;
        }
        return true;
    case PIK_FRAME_CTRL_COMMAND:
        if (len != 5) {
            LOG("bad COMMAND len=%zu", len);
            return false;
        }
        {
            pik_control_action_t action = (pik_control_action_t)p[4];
            uint32_t request_id = pik_get_u32le(p);
            if (!action_valid(action)) {
                LOG("rejecting unknown command action=%u request=%u", p[4], request_id);
                return pik_control_send_ack(request_id, PIK_CONTROL_ACK_UNKNOWN_ACTION,
                                            NULL, 0);
            }
            if (g_ctrl.on_command)
                g_ctrl.on_command(action, request_id);
            if (pik_session_link()->failed)
                return false;
        }
        return true;
    case PIK_FRAME_CTRL_ACK:
        if (len < 5 || len > PIK_CTRL_MAX_PAYLOAD) {
            LOG("bad ACK len=%zu", len);
            return false;
        }
        if (g_ctrl.ack_pending) {
            LOG("received ACK before previous ACK was consumed");
            return false;
        }
        g_ctrl.ack_request_id = pik_get_u32le(p);
        g_ctrl.ack_status = (pik_control_ack_status_t)p[4];
        if (!ack_status_valid(g_ctrl.ack_status)) {
            LOG("bad ACK status=%u request=%u", p[4], g_ctrl.ack_request_id);
            return false;
        }
        g_ctrl.ack_payload_len = len - 5;
        if (g_ctrl.ack_payload_len)
            memcpy(g_ctrl.ack_payload, p + 5, g_ctrl.ack_payload_len);
        g_ctrl.ack_pending = true;
        return true;
    case PIK_FRAME_CTRL_CONFIG:
        return handle_config(p, len);
    default:
        LOG("bad control frame type=0x%02x len=%zu", type, len);
        return false;
    }
}

void pik_control_on_link_down(void) {
    if (g_ctrl.ready) LOG("link down");
    g_ctrl.ready = false;
    clear_pending_ack();
}

void pik_control_init(pik_control_role_t role,
                      pik_control_command_fn on_command,
                      pik_control_ready_fn on_ready) {
    memset(&g_ctrl, 0, sizeof(g_ctrl));
    g_ctrl.role = role;
    g_ctrl.on_command = on_command;
    g_ctrl.on_ready = on_ready;
    g_ctrl.next_request_id = 1;
}

bool pik_control_on_link_open(void) {
    g_ctrl.ready = false;
    return send_hello();
}

bool pik_control_tick(int64_t now) {
    pik_link_t *lk = pik_session_link();
    if (!pik_link_is_open(lk)) return false;
    if (!g_ctrl.ready) {
        if (!pik_link_tx_avail(lk) &&
            (now - lk->last_tx_ms) > HELLO_RETRY_MS)
            send_hello();
        if ((now - lk->last_rx_ms) > LINK_DEAD_MS) {
            g_ctrl.handshake_fails++;
            if (g_ctrl.handshake_fails == 1) {
                LOG("handshake RX timeout");
                g_ctrl.handshake_report_ms = now;
            } else if (now - g_ctrl.handshake_report_ms >= HANDSHAKE_REPORT_MS) {
                LOG("handshake still failing: %u attempts in the last %llds",
                    g_ctrl.handshake_fails - g_ctrl.handshake_fails_reported,
                    (long long)((now - g_ctrl.handshake_report_ms) / 1000));
                g_ctrl.handshake_report_ms = now;
                g_ctrl.handshake_fails_reported = g_ctrl.handshake_fails;
            }
            pik_session_fail();
            return false;
        }
    } else {
        if (!pik_link_tx_avail(lk) &&
            (now - lk->last_tx_ms) > PING_IDLE_MS)
            enqueue_frame(PIK_FRAME_CTRL_PING, NULL, 0);
        if ((now - lk->last_rx_ms) > LINK_DEAD_MS) {
            LOG("RX timeout");
            pik_session_fail();
            return false;
        }
    }
    return !lk->failed;
}

uint32_t pik_control_handshake_failures(void) {
    return g_ctrl.handshake_fails;
}

int64_t pik_control_deadline(void) {
    const pik_link_t *lk = pik_session_link();
    if (!pik_link_is_open(lk)) return INT64_MAX;
    int64_t a, b;
    if (!g_ctrl.ready) {
        a = lk->last_tx_ms + HELLO_RETRY_MS;
        b = lk->last_rx_ms + LINK_DEAD_MS;
    } else {
        a = lk->last_tx_ms + PING_IDLE_MS;
        b = lk->last_rx_ms + LINK_DEAD_MS;
    }
    return a < b ? a : b;
}

void pik_control_cleanup(void) {
    g_ctrl.ready = false;
    clear_pending_ack();
}

void pik_control_set_config(const uint8_t *channels, size_t n_channels,
                            pik_control_tcp_role_t tcp_role) {
    if (n_channels > sizeof(g_ctrl.local_channels))
        n_channels = sizeof(g_ctrl.local_channels);
    if (n_channels)
        memcpy(g_ctrl.local_channels, channels, n_channels);
    g_ctrl.local_channel_count = n_channels;
    g_ctrl.local_tcp_role = tcp_role;
}

bool pik_control_send_command(pik_control_action_t action, uint32_t *request_id) {
    if (!g_ctrl.ready) return false;
    uint8_t p[5];
    uint32_t id = g_ctrl.next_request_id++;
    if (g_ctrl.next_request_id == 0) g_ctrl.next_request_id = 1;
    pik_put_u32le(p, id);
    p[4] = (uint8_t)action;
    if (!enqueue_frame(PIK_FRAME_CTRL_COMMAND, p, sizeof(p))) return false;
    if (request_id) *request_id = id;
    LOG("sent command %s request=%u", pik_control_action_name(action), id);
    return true;
}

bool pik_control_take_ack(uint32_t *request_id, pik_control_ack_status_t *status,
                          const uint8_t **payload, size_t *payload_len) {
    if (!g_ctrl.ack_pending) return false;
    if (request_id) *request_id = g_ctrl.ack_request_id;
    if (status) *status = g_ctrl.ack_status;
    if (payload) *payload = g_ctrl.ack_payload;
    if (payload_len) *payload_len = g_ctrl.ack_payload_len;
    g_ctrl.ack_pending = false;
    return true;
}

bool pik_control_send_ack(uint32_t request_id, pik_control_ack_status_t status,
                          const uint8_t *payload, size_t payload_len) {
    uint8_t p[PIK_CTRL_MAX_PAYLOAD];
    if (payload_len > PIK_CTRL_ACK_MAX_PAYLOAD)
        return false;
    pik_put_u32le(p, request_id);
    p[4] = status;
    if (payload_len)
        memcpy(p + 5, payload, payload_len);
    return enqueue_frame(PIK_FRAME_CTRL_ACK, p, 5 + payload_len);
}
