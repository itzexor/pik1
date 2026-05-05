#pragma once
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
    char       vidpid[16];
    ch_spec_t  channels[MAX_CHANNELS];
    int        n_channels;

    // Optional hooks for pik1d child-process management.
    // Set unused fields to -1 / NULL.
    int        aux_fd;           // signalfd to watch in epoll, or -1
    void     (*aux_cb)(void);    // called when aux_fd fires
    void     (*tick_cb)(void);   // called every event-loop iteration
    int64_t  (*deadline_fn)(void); // extra deadline contribution, or NULL
} serialmux_config_t;

// Find the nth (0-indexed) USB serial device matching vidpid ("VID:PID").
// Returns a pointer to a static buffer, or NULL if not found.
const char *serialmux_find_dev(const char *vidpid, int n);

// Run the mux event loop forever using the given config.
void serialmux_run(const serialmux_config_t *cfg);
