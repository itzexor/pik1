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

const char *pik_local_control_sock_path(void);
int pik_local_control_client_main(const char *cmd);

bool pik_local_control_start(int epfd);
void pik_local_control_cleanup(void);
bool pik_local_control_owns_listen(const void *ptr);
bool pik_local_control_owns_client(const void *ptr);
void pik_local_control_accept(void);
void pik_local_control_read(int64_t now, bool command_busy);

bool pik_local_control_pending(void);
uint32_t pik_local_control_request_id(void);
void pik_local_control_complete(pik_control_ack_status_t status,
                                const uint8_t *payload, size_t payload_len);
void pik_local_control_check_timeout(int64_t now);
int64_t pik_local_control_deadline(void);

bool pik_parse_control_action(const char *cmd, pik_control_action_t *action);
void pik_mark_peer_initiated(void);
