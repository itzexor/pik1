#pragma once

#include "control.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef PIK_LOCAL_CONTROL_DIR
#define PIK_LOCAL_CONTROL_DIR "/run/pik1"
#endif
#ifndef PIK_LOCAL_CONTROL_SOCK
#define PIK_LOCAL_CONTROL_SOCK PIK_LOCAL_CONTROL_DIR "/control.sock"
#endif
#ifndef PIK_PEER_INITIATED_MARKER
#define PIK_PEER_INITIATED_MARKER PIK_LOCAL_CONTROL_DIR "/peer-initiated"
#endif

#define PIK_CONTROL_SOCK_ENV "PIK1_CONTROL_SOCK"
#define PIK_COMMAND_ACK_TIMEOUT_MS 3000
#define PIK_REMOTE_ACTION_DELAY_MS 250

const char *pik_commands_sock_path(void);
int pik_commands_client_main(const char *cmd);

bool pik_commands_start(int epfd);
void pik_commands_cleanup(void);
bool pik_commands_owns_event(const void *ptr);
void pik_commands_dispatch(void *ptr, int64_t now);

bool pik_commands_parse_action(const char *cmd, pik_control_action_t *action);
void pik_commands_mark_peer_initiated(void);

void pik_commands_init(const char *side_name);
void pik_commands_set_service_flags(uint32_t flags);
void pik_commands_set_service_flag(uint32_t flag, bool up);
void pik_commands_on_command(pik_control_action_t action,
                             uint32_t request_id);
void pik_commands_check_acks(int64_t now);
int64_t pik_commands_deadline(void);
bool pik_commands_signal_pending(void);
bool pik_commands_signal_done(void);
void pik_commands_request_restart_peer(bool can_signal_peer_restart, int64_t now);
bool pik_commands_action_due(int64_t now, pik_control_action_t *action);
