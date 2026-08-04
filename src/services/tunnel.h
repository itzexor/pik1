#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* TCP tunnel service: multiplexes TCP connections over channel 15 of the
 * shared link. listen mode accepts local connections and OPENs streams;
 * forward mode dials the target when an OPEN arrives. Stream frames carry
 * a [conn:1][gen:1] payload prefix; the generation byte disambiguates a
 * reused conn slot from in-flight frames of its previous incarnation. */

typedef enum {
    TUNNEL_MODE_NONE = 0,
    TUNNEL_MODE_LISTEN,
    TUNNEL_MODE_FORWARD,
} tunnel_mode_t;

void tunnel_init(int epfd, tunnel_mode_t mode, const char *host, int port);
/* Session handshake completed: bind the listener (listen mode). */
void tunnel_start(int64_t now);
/* Inbound tunnel frame from the session router. */
bool tunnel_on_frame(uint8_t type, const uint8_t *payload, size_t plen);
void tunnel_on_link_down(void);
bool tunnel_owns_event(const void *ptr);
bool tunnel_dispatch(void *ptr, uint32_t events);
bool tunnel_tick(int64_t now);
int64_t tunnel_deadline(void);
/* Ready to carry streams (forward mode: started; listen mode: listening). */
bool tunnel_active(void);
void tunnel_cleanup(void);
