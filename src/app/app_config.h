#pragma once

#include "control.h"
#include "serialmux.h"
#include "tunnel.h"

typedef enum {
    APP_MODE_K1,
    APP_MODE_PI,
} app_mode_t;

typedef struct {
    app_mode_t mode;
    serialmux_config_t mux;
    char tcp_addr[64];
    int tcp_port;
    tunnel_mode_t tunnel_mode;
    pik_control_tcp_role_t tcp_role;
} app_config_t;

_Noreturn void pik_app_config_usage(const char *prog);
void pik_app_config_parse(int argc, char **argv, app_config_t *cfg);
