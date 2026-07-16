#pragma once
#include "pik_proto.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MAX_CHANNELS PIK_MUX_CLI_COUNT

typedef enum { CH_MCU, CH_PTY } ch_type_t;

typedef struct {
    ch_type_t type;
    uint8_t   ch_id;      // user-facing CLI channel id
    char      dev[128];   // MCU: serial device path
    int       baud;       // MCU: baud rate
    char      path[128];  // PTY: symlink path
} ch_spec_t;

typedef struct {
    ch_spec_t  channels[MAX_CHANNELS];
    int        n_channels;
} serialmux_config_t;

void serialmux_init(const serialmux_config_t *cfg, int epfd);
/* Session handshake completed: reset channel state and announce MCUs. */
void serialmux_start(int64_t now);
/* Inbound mux frame from the session router (wire channel id). */
bool serialmux_on_frame(uint8_t type, uint8_t wire_ch,
                        const uint8_t *payload, size_t plen);
void serialmux_on_link_down(void);
bool serialmux_owns_event(const void *ptr);
bool serialmux_dispatch(void *ptr, uint32_t events, int64_t now);
bool serialmux_tick(int64_t now);
int64_t serialmux_deadline(int64_t now);
void serialmux_cleanup(void);
