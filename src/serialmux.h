#pragma once
#include <stdbool.h>
#include <stdint.h>

#define MAX_CHANNELS 8

typedef enum { CH_MCU, CH_PTY } ch_type_t;

typedef struct {
    ch_type_t type;
    uint8_t   ch_id;
    char      dev[128];   // MCU: serial device path
    int       baud;       // MCU: baud rate
    char      path[128];  // PTY: symlink path
} ch_spec_t;

typedef struct {
    ch_spec_t  channels[MAX_CHANNELS];
    int        n_channels;
} serialmux_config_t;

void serialmux_init(const serialmux_config_t *cfg, int epfd);
bool serialmux_start(const char *link_dev, int64_t now);
bool serialmux_dispatch(void *ptr, uint32_t events, int64_t now);
bool serialmux_tick(int64_t now);
int64_t serialmux_deadline(int64_t now);
void serialmux_cleanup(void);
