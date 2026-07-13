#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    PIK_CONTROL_ROLE_PTY = 1,
    PIK_CONTROL_ROLE_MCU = 2,
} pik_control_role_t;

typedef enum {
    PIK_CONTROL_ACTION_RESTART_PEER = 1,
    PIK_CONTROL_ACTION_REBOOT_PEER = 2,
    PIK_CONTROL_ACTION_POWEROFF_PEER = 3,
    PIK_CONTROL_ACTION_STATUS = 4,
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
    PIK_CONTROL_LINK_SERIAL = 1u << 0,
    PIK_CONTROL_LINK_TCP    = 1u << 1,
};

typedef void (*pik_control_command_fn)(pik_control_action_t action,
                                       uint32_t request_id,
                                       void *ctx);

void pik_control_init(int epfd, pik_control_role_t role,
                      pik_control_command_fn on_command, void *ctx);
bool pik_control_start(const char *dev, int64_t now);
bool pik_control_dispatch(void *ptr, uint32_t events, int64_t now);
bool pik_control_owns_event(void *ptr);
bool pik_control_tick(int64_t now);
bool pik_control_ready(void);
/* Consecutive handshake RX timeouts since the last successful handshake; the
 * supervisor uses this to quiet its per-attempt logging in a dead-peer loop. */
uint32_t pik_control_handshake_failures(void);
int64_t pik_control_deadline(void);
void pik_control_cleanup(void);

void pik_control_set_config(const uint8_t *channels, size_t n_channels,
                            pik_control_tcp_role_t tcp_role);
/* One outstanding outbound command per daemon: the pending-ACK slot is
 * single, so callers (local socket, signal path) must not overlap requests. */
bool pik_control_send_command(pik_control_action_t action, uint32_t *request_id);
bool pik_control_take_ack(uint32_t *request_id, pik_control_ack_status_t *status,
                          const uint8_t **payload, size_t *payload_len);
bool pik_control_send_ack(uint32_t request_id, pik_control_ack_status_t status,
                          const uint8_t *payload, size_t payload_len);
bool pik_control_send_link_state(uint32_t flags);
bool pik_control_peer_link_state(uint32_t *flags);

const char *pik_control_action_name(pik_control_action_t action);
const char *pik_control_ack_status_name(pik_control_ack_status_t status);
