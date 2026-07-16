#pragma once

#include "control.h"
#include "serialmux.h"
#include "tunnel.h"

#include <stdbool.h>

typedef enum {
    APP_MODE_K1,
    APP_MODE_PI,
} app_mode_t;

typedef struct {
    app_mode_t mode;
    const char *uart_name;
    pik_control_role_t control_role;
    serialmux_config_t mux;
    bool has_tcp;
    const char *tcp_mode_name;
    char tcp_addr[64];
    int tcp_port;
    tunnel_mode_t tunnel_mode;
    pik_control_tcp_role_t tcp_role;
} app_config_t;

void pik_app_config_usage(const char *prog);
void pik_app_config_parse(int argc, char **argv, app_config_t *cfg);
