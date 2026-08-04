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

typedef enum {
    PIK_CONTROL_ACTION_RESTART_PEER = 1,
    PIK_CONTROL_ACTION_REBOOT_PEER = 2,
    PIK_CONTROL_ACTION_POWEROFF_PEER = 3,
    PIK_CONTROL_ACTION_STATUS = 4,
    PIK_CONTROL_ACTION_WIFI_RESET_PEER = 5,
} pik_control_action_t;

typedef enum {
    PIK_CONTROL_ACK_OK = 0,
    PIK_CONTROL_ACK_UNKNOWN_ACTION = 1,
    PIK_CONTROL_ACK_INTERNAL_ERROR = 2,
} pik_control_ack_status_t;

typedef enum {
    PIK_CONTROL_TCP_NONE = 0,
    PIK_CONTROL_TCP_LISTEN = 1,
    PIK_CONTROL_TCP_FORWARD = 2,
} pik_control_tcp_role_t;

enum {
    PIK_CONTROL_SERVICE_SERIAL = 1u << 0,
    PIK_CONTROL_SERVICE_TUNNEL = 1u << 1,
};

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
bool pik_control_ready(void);
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
bool pik_control_send_service_state(uint32_t flags);
bool pik_control_peer_service_state(uint32_t *flags);

const char *pik_control_action_name(pik_control_action_t action);
const char *pik_control_ack_status_name(pik_control_ack_status_t status);
