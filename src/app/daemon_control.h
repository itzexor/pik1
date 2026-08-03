#pragma once

#include "control.h"

#include <stdbool.h>
#include <stdint.h>

#define PIK_REMOTE_ACTION_DELAY_MS 250

void pik_daemon_control_init(const char *uart_name);
void pik_daemon_set_service_flags(uint32_t flags);
void pik_daemon_set_service_flag(uint32_t flag, bool up);

void pik_daemon_on_control_command(pik_control_action_t action, uint32_t request_id);
void pik_daemon_check_acks(int64_t now);
int64_t pik_daemon_control_deadline(void);

bool pik_daemon_signal_pending(void);
bool pik_daemon_signal_done(void);
void pik_daemon_signal_mark_done(void);
void pik_daemon_request_restart_peer(bool can_signal_peer_restart, int64_t now);

bool pik_daemon_remote_action_due(int64_t now, pik_control_action_t *action);
