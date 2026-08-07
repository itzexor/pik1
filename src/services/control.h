#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Control service: HELLO handshake, link liveness, and peer commands on
 * channel 0 of the shared link (pik_proto.h / session.h). */

typedef enum {
    PIK_CONTROL_ROLE_PTY = 1,
    PIK_CONTROL_ROLE_MCU = 2,
} pik_control_role_t;

/* Each vocabulary below is one list of [symbol, wire value, name]: the enum,
 * the name lookups, the CLI parser, the validity checks and the usage text
 * are all generated from it, so a new entry is added in exactly one place. */
#define PIK_CONTROL_ENUM_ENTRY(sym, value, name) sym = (value),

#define PIK_CONTROL_ACTION_LIST(X) \
    X(PIK_CONTROL_ACTION_RESTART_PIK1,    1, "restart-pik1")    \
    X(PIK_CONTROL_ACTION_REBOOT,          2, "reboot")          \
    X(PIK_CONTROL_ACTION_POWEROFF,        3, "poweroff")        \
    X(PIK_CONTROL_ACTION_STATUS,          4, "status")          \
    X(PIK_CONTROL_ACTION_RESTART_WIFI,    5, "restart-wifi")    \
    X(PIK_CONTROL_ACTION_RESTART_KLIPPER, 6, "restart-klipper")

#define PIK_CONTROL_ACK_LIST(X) \
    X(PIK_CONTROL_ACK_OK,             0, "ok")              \
    X(PIK_CONTROL_ACK_UNKNOWN_ACTION, 1, "unknown-action")  \
    X(PIK_CONTROL_ACK_INTERNAL_ERROR, 2, "internal-error")

#define PIK_CONTROL_TCP_LIST(X) \
    X(PIK_CONTROL_TCP_NONE,    0, "none")     \
    X(PIK_CONTROL_TCP_LISTEN,  1, "listen")   \
    X(PIK_CONTROL_TCP_FORWARD, 2, "forward")

#define PIK_CONTROL_SERVICE_LIST(X) \
    X(PIK_CONTROL_SERVICE_SERIAL, 1u << 0, "serial")  \
    X(PIK_CONTROL_SERVICE_TUNNEL, 1u << 1, "tunnel")

typedef enum {
    PIK_CONTROL_ACTION_LIST(PIK_CONTROL_ENUM_ENTRY)
} pik_control_action_t;

typedef enum {
    PIK_CONTROL_ACK_LIST(PIK_CONTROL_ENUM_ENTRY)
} pik_control_ack_status_t;

typedef enum {
    PIK_CONTROL_TCP_LIST(PIK_CONTROL_ENUM_ENTRY)
} pik_control_tcp_role_t;

enum {
    PIK_CONTROL_SERVICE_LIST(PIK_CONTROL_ENUM_ENTRY)
};

#undef PIK_CONTROL_ENUM_ENTRY

/* Longest "a,b,..." join of the service names, plus terminator. */
#define PIK_CONTROL_SERVICE_NAMES_MAX 32u

typedef void (*pik_control_command_fn)(pik_control_action_t action,
                                       uint32_t request_id);
/* Fired once per session when the HELLO handshake completes, before any
 * mux/tunnel frame from the peer can be delivered (in-order link). */
typedef void (*pik_control_ready_fn)(void);

void pik_control_init(pik_control_role_t role,
                      pik_control_command_fn on_command,
                      pik_control_ready_fn on_ready);
/* Send the opening HELLO; call right after the transport begins the link. */
bool pik_control_on_link_open(void);
/* Inbound control frame from the session router. */
bool pik_control_on_frame(uint8_t type, const uint8_t *payload, size_t plen);
/* Session went down: reset per-session state (keeps handshake failure count). */
void pik_control_on_link_down(void);
bool pik_control_tick(int64_t now);
/* Consecutive handshake RX timeouts since the last successful handshake. */
uint32_t pik_control_handshake_failures(void);
int64_t pik_control_deadline(void);
void pik_control_cleanup(void);

/* Set once before opening the link; each handshake sends this configuration. */
void pik_control_set_config(const uint8_t *channels, size_t n_channels,
                            pik_control_tcp_role_t tcp_role);
/* One outstanding outbound command per daemon: the pending-ACK slot is
 * single, so callers (local socket, signal path) must not overlap requests. */
bool pik_control_send_command(pik_control_action_t action, uint32_t *request_id);
bool pik_control_take_ack(uint32_t *request_id, pik_control_ack_status_t *status,
                          const uint8_t **payload, size_t *payload_len);
bool pik_control_send_ack(uint32_t request_id, pik_control_ack_status_t status,
                          const uint8_t *payload, size_t payload_len);

/* Vocabulary lookups (control_names.c); the *_name calls return a fixed
 * placeholder for values outside their list so they are safe to log. */
const char *pik_control_action_name(pik_control_action_t action);
bool pik_control_parse_action(const char *name, pik_control_action_t *action);
bool pik_control_action_valid(pik_control_action_t action);
const char *pik_control_ack_status_name(pik_control_ack_status_t status);
bool pik_control_ack_status_valid(pik_control_ack_status_t status);
const char *pik_control_tcp_role_name(pik_control_tcp_role_t role);
bool pik_control_tcp_role_valid(pik_control_tcp_role_t role);
/* Comma-joined names of the services set in flags, or "none"; writes at most
 * cap bytes including the terminator (PIK_CONTROL_SERVICE_NAMES_MAX fits). */
void pik_control_service_names(uint32_t flags, char *buf, size_t cap);
