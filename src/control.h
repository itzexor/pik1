#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    PIK_CONTROL_ROLE_HOST = 1,
    PIK_CONTROL_ROLE_EXPORTER = 2,
} pik_control_role_t;

typedef enum {
    PIK_CONTROL_ACTION_RESTART_EXPORTER = 1,
    PIK_CONTROL_ACTION_REBOOT_EXPORTER = 2,
    PIK_CONTROL_ACTION_POWEROFF_EXPORTER = 3,
} pik_control_action_t;

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
int64_t pik_control_deadline(void);
void pik_control_cleanup(void);

bool pik_control_send_command(pik_control_action_t action, uint32_t *request_id);
bool pik_control_take_ack(uint32_t *request_id, uint8_t *status);
void pik_control_send_ack(uint32_t request_id, uint8_t status);

const char *pik_control_action_name(pik_control_action_t action);
