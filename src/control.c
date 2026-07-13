#include "control.h"
#include "control_proto.h"
#include "nanocobs/cobs.h"
#include "frame.h"
#include "link.h"
#include "logging.h"
#include "util.h"
#include "version.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define FRAME_DEC_MAX (PIK_CONTROL_FRAME_HEADER_LEN + PIK_CONTROL_MAX_PAYLOAD + 4u)
#define FRAME_ENC_MAX COBS_ENCODE_MAX(FRAME_DEC_MAX)

#define TX_RING_CAP   8192u
#define HIST_CAP      4096u
#define HIST_SLOTS    64u

#define HELLO_RETRY_MS 2000
#define PING_IDLE_MS  3000
#define LINK_DEAD_MS  10000

/* While the peer stays unreachable the handshake fails every LINK_DEAD_MS;
 * log the first failure, then one summary line per this interval. */
#define HANDSHAKE_REPORT_MS 600000

typedef struct {
    pik_link_t lk;

    pik_control_role_t role;
    pik_control_command_fn on_command;
    void *ctx;
    bool ready;

    uint32_t next_request_id;
    bool ack_pending;
    uint32_t ack_request_id;
    pik_control_ack_status_t ack_status;
    uint8_t ack_payload[PIK_CONTROL_MAX_PAYLOAD - 5u];
    size_t ack_payload_len;

    bool peer_link_known;
    uint32_t peer_link_flags;

    uint8_t local_channels[PIK_CONTROL_MAX_PAYLOAD - 2u];
    size_t local_channel_count;
    pik_control_tcp_role_t local_tcp_role;
    bool config_sent;

    uint32_t handshake_fails;          /* consecutive handshake RX timeouts */
    uint32_t handshake_fails_reported;
    int64_t  handshake_report_ms;
} control_t;

static control_t g_ctrl;

static uint8_t             s_txbuf[TX_RING_CAP];
static uint8_t             s_rxbuf[FRAME_ENC_MAX + 4];
static uint8_t             s_hist[HIST_CAP];
static pik_link_hist_ent_t s_hist_ent[HIST_SLOTS];

#define LOG(...) pik_log("ctrl", __VA_ARGS__)

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
    default: return "unknown-status";
    }
}

static bool action_valid(pik_control_action_t action) {
    switch (action) {
    case PIK_CONTROL_ACTION_RESTART_PEER:
    case PIK_CONTROL_ACTION_REBOOT_PEER:
    case PIK_CONTROL_ACTION_POWEROFF_PEER:
    case PIK_CONTROL_ACTION_STATUS:
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
    return pik_link_enqueue(&g_ctrl.lk, type, 0, payload, plen);
}

static bool send_hello(void) {
    uint8_t p[PIK_CONTROL_HELLO_LEN];
    memset(p, 0, sizeof(p));
    p[0] = PIK_CONTROL_HELLO_MAGIC_0;
    p[1] = PIK_CONTROL_HELLO_MAGIC_1;
    p[2] = PIK_CONTROL_HELLO_MAGIC_2;
    p[3] = PIK_CONTROL_HELLO_MAGIC_3;
    pik_put_u32le(p + 4, PIK1_PROTOCOL_VERSION);
    pik_put_u32le(p + 8, PIK1_FEATURE_FLAGS);
    p[12] = (uint8_t)g_ctrl.role;
    snprintf((char *)p + 13, PIK_CONTROL_RELEASE_LEN, "%s", PIK1_RELEASE_VERSION);
    return enqueue_frame(PIK_CONTROL_FRAME_HELLO, p, sizeof(p));
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
    uint8_t p[PIK_CONTROL_MAX_PAYLOAD];
    if (g_ctrl.local_channel_count > sizeof(p) - 2)
        return false;
    p[0] = (uint8_t)g_ctrl.local_tcp_role;
    p[1] = (uint8_t)g_ctrl.local_channel_count;
    if (g_ctrl.local_channel_count)
        memcpy(p + 2, g_ctrl.local_channels, g_ctrl.local_channel_count);
    if (!enqueue_frame(PIK_CONTROL_FRAME_CONFIG, p, g_ctrl.local_channel_count + 2))
        return false;
    g_ctrl.config_sent = true;
    return true;
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
        if (peer_present[id]) continue;
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
    if (len != PIK_CONTROL_HELLO_LEN ||
        p[0] != PIK_CONTROL_HELLO_MAGIC_0 || p[1] != PIK_CONTROL_HELLO_MAGIC_1 ||
        p[2] != PIK_CONTROL_HELLO_MAGIC_2 || p[3] != PIK_CONTROL_HELLO_MAGIC_3) {
        LOG("bad HELLO");
        return false;
    }

    uint32_t proto = pik_get_u32le(p + 4);
    uint32_t features = pik_get_u32le(p + 8);
    uint8_t role = p[12];
    char release[PIK_CONTROL_RELEASE_LEN + 1];
    memcpy(release, p + 13, PIK_CONTROL_RELEASE_LEN);
    release[PIK_CONTROL_RELEASE_LEN] = '\0';

    if (proto != PIK1_PROTOCOL_VERSION) {
        LOG("protocol mismatch: local=%u remote=%u remote_release=%s",
            PIK1_PROTOCOL_VERSION, proto, release);
        return false;
    }
    if (role == (uint8_t)g_ctrl.role) {
        LOG("peer role mismatch: both sides role=%u", role);
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
            g_ctrl.lk.quiet = false;
        }
        LOG("link up: release=%s protocol=%u features=0x%08x",
            release, proto, features);
        if (!send_hello())
            return false;
        if (!send_config())
            return false;
    }
    return true;
}

static bool ctrl_on_frame(void *ctx, uint8_t type, uint8_t aux,
                          const uint8_t *p, size_t len) {
    (void)ctx;
    (void)aux;

    switch (type) {
    case PIK_CONTROL_FRAME_HELLO:
        return handle_hello(p, len);
    case PIK_CONTROL_FRAME_PING:
        if (len != 0) {
            LOG("bad PING len=%zu", len);
            return false;
        }
        return enqueue_frame(PIK_CONTROL_FRAME_PONG, NULL, 0);
    case PIK_CONTROL_FRAME_PONG:
        if (len != 0) {
            LOG("bad PONG len=%zu", len);
            return false;
        }
        return true;
    case PIK_CONTROL_FRAME_COMMAND:
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
                g_ctrl.on_command(action, request_id, g_ctrl.ctx);
            if (g_ctrl.lk.failed)
                return false;
        }
        return true;
    case PIK_CONTROL_FRAME_ACK:
        if (len < 5) {
            LOG("bad ACK len=%zu", len);
            return false;
        }
        g_ctrl.ack_request_id = pik_get_u32le(p);
        g_ctrl.ack_status = (pik_control_ack_status_t)p[4];
        if (!ack_status_valid(g_ctrl.ack_status)) {
            LOG("bad ACK status=%u request=%u", p[4], g_ctrl.ack_request_id);
            return false;
        }
        g_ctrl.ack_payload_len = len - 5;
        if (g_ctrl.ack_payload_len > sizeof(g_ctrl.ack_payload))
            g_ctrl.ack_payload_len = sizeof(g_ctrl.ack_payload);
        if (g_ctrl.ack_payload_len)
            memcpy(g_ctrl.ack_payload, p + 5, g_ctrl.ack_payload_len);
        g_ctrl.ack_pending = true;
        return true;
    case PIK_CONTROL_FRAME_LINK_STATE:
        if (len != 4) {
            LOG("bad LINK_STATE len=%zu", len);
            return false;
        }
        g_ctrl.peer_link_flags = pik_get_u32le(p);
        g_ctrl.peer_link_known = true;
        LOG("peer data links: serial=%s tcp=%s",
            (g_ctrl.peer_link_flags & PIK_CONTROL_LINK_SERIAL) ? "up" : "down",
            (g_ctrl.peer_link_flags & PIK_CONTROL_LINK_TCP) ? "up" : "down");
        return true;
    case PIK_CONTROL_FRAME_CONFIG:
        return handle_config(p, len);
    default:
        LOG("bad control frame type=0x%02x len=%zu", type, len);
        return false;
    }
}

static void ctrl_on_down(void *ctx) {
    (void)ctx;
    if (g_ctrl.ready) LOG("link down");
    g_ctrl.ready = false;
    clear_pending_ack();
}

void pik_control_init(int epfd, pik_control_role_t role,
                      pik_control_command_fn on_command, void *ctx) {
    memset(&g_ctrl, 0, sizeof(g_ctrl));
    g_ctrl.role = role;
    g_ctrl.on_command = on_command;
    g_ctrl.ctx = ctx;
    g_ctrl.next_request_id = 1;

    pik_link_cfg_t cfg = {
        .name        = "ctrl",
        .nak_type    = PIK_CONTROL_FRAME_NAK,
        .has_aux     = false,
        .first_type  = PIK_CONTROL_FRAME_HELLO,
        .heal_from_zero = true, /* handshake frames are idempotent */
        .max_payload = PIK_CONTROL_MAX_PAYLOAD,
        .txbuf       = s_txbuf,    .tx_cap     = TX_RING_CAP,
        .rxbuf       = s_rxbuf,    .rx_cap     = sizeof(s_rxbuf),
        .hist        = s_hist,     .hist_cap   = HIST_CAP,
        .hist_ent    = s_hist_ent, .hist_slots = HIST_SLOTS,
        .on_frame    = ctrl_on_frame,
        .on_down     = ctrl_on_down,
        .ctx         = NULL,
    };
    pik_link_init(&g_ctrl.lk, &cfg, epfd);
}

bool pik_control_start(const char *dev, int64_t now) {
    g_ctrl.lk.quiet = g_ctrl.handshake_fails > 0;
    if (!pik_link_open(&g_ctrl.lk, dev, now))
        return false;
    g_ctrl.ready = false;
    g_ctrl.config_sent = false;
    return send_hello();
}

bool pik_control_owns_event(void *ptr) {
    return pik_link_owns_event(&g_ctrl.lk, ptr);
}

bool pik_control_dispatch(void *ptr, uint32_t events, int64_t now) {
    if (!pik_control_owns_event(ptr)) return true;
    return pik_link_dispatch(&g_ctrl.lk, events, now);
}

bool pik_control_tick(int64_t now) {
    if (!pik_link_tick(&g_ctrl.lk, now))
        return false;
    if (!g_ctrl.ready) {
        if (!pik_link_tx_avail(&g_ctrl.lk) &&
            (now - g_ctrl.lk.last_tx_ms) > HELLO_RETRY_MS)
            send_hello();
        if ((now - g_ctrl.lk.last_rx_ms) > LINK_DEAD_MS) {
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
            pik_link_fail(&g_ctrl.lk);
            return false;
        }
    } else {
        if (!g_ctrl.config_sent)
            send_config();
        if (!pik_link_tx_avail(&g_ctrl.lk) &&
            (now - g_ctrl.lk.last_tx_ms) > PING_IDLE_MS)
            enqueue_frame(PIK_CONTROL_FRAME_PING, NULL, 0);
        if ((now - g_ctrl.lk.last_rx_ms) > LINK_DEAD_MS) {
            LOG("RX timeout");
            pik_link_fail(&g_ctrl.lk);
            return false;
        }
    }
    return !g_ctrl.lk.failed;
}

bool pik_control_ready(void) {
    return g_ctrl.ready;
}

uint32_t pik_control_handshake_failures(void) {
    return g_ctrl.handshake_fails;
}

int64_t pik_control_deadline(void) {
    if (g_ctrl.lk.fd < 0) return INT64_MAX;
    int64_t a, b;
    if (!g_ctrl.ready) {
        a = g_ctrl.lk.last_tx_ms + HELLO_RETRY_MS;
        b = g_ctrl.lk.last_rx_ms + LINK_DEAD_MS;
    } else {
        a = g_ctrl.lk.last_tx_ms + PING_IDLE_MS;
        b = g_ctrl.lk.last_rx_ms + LINK_DEAD_MS;
    }
    int64_t dl = a < b ? a : b;
    int64_t c = pik_link_deadline(&g_ctrl.lk);
    return c < dl ? c : dl;
}

void pik_control_cleanup(void) {
    pik_link_cleanup(&g_ctrl.lk);
    g_ctrl.ready = false;
    g_ctrl.config_sent = false;
    clear_pending_ack();
    g_ctrl.peer_link_known = false;
    g_ctrl.peer_link_flags = 0;
}

void pik_control_set_config(const uint8_t *channels, size_t n_channels,
                            pik_control_tcp_role_t tcp_role) {
    if (n_channels > sizeof(g_ctrl.local_channels))
        n_channels = sizeof(g_ctrl.local_channels);
    if (n_channels)
        memcpy(g_ctrl.local_channels, channels, n_channels);
    g_ctrl.local_channel_count = n_channels;
    g_ctrl.local_tcp_role = tcp_role;
    g_ctrl.config_sent = false;
    if (g_ctrl.ready)
        send_config();
}

bool pik_control_send_command(pik_control_action_t action, uint32_t *request_id) {
    if (!g_ctrl.ready) return false;
    uint8_t p[5];
    uint32_t id = g_ctrl.next_request_id++;
    if (g_ctrl.next_request_id == 0) g_ctrl.next_request_id = 1;
    pik_put_u32le(p, id);
    p[4] = (uint8_t)action;
    if (!enqueue_frame(PIK_CONTROL_FRAME_COMMAND, p, sizeof(p))) return false;
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
    uint8_t p[PIK_CONTROL_MAX_PAYLOAD];
    if (payload_len > sizeof(p) - 5)
        payload_len = sizeof(p) - 5;
    pik_put_u32le(p, request_id);
    p[4] = status;
    if (payload_len)
        memcpy(p + 5, payload, payload_len);
    return enqueue_frame(PIK_CONTROL_FRAME_ACK, p, 5 + payload_len);
}

bool pik_control_send_link_state(uint32_t flags) {
    uint8_t p[4];
    pik_put_u32le(p, flags);
    return enqueue_frame(PIK_CONTROL_FRAME_LINK_STATE, p, sizeof(p));
}

bool pik_control_peer_link_state(uint32_t *flags) {
    if (!g_ctrl.peer_link_known) return false;
    if (flags) *flags = g_ctrl.peer_link_flags;
    return true;
}
