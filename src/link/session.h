#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "link.h"

/* Owner of the single shared link (pik_proto.h): opens/fails the pik_link,
 * routes inbound frames to the control/mux/tunnel services by type nibble,
 * and schedules outbound frames through per-service queues so the wire FIFO
 * stays shallow and control/MCU traffic is never stuck behind a tunnel
 * burst. Strict priority: control >= mux > tunnel. */

typedef enum {
    PIK_SESSION_CLASS_CONTROL = 0,
    PIK_SESSION_CLASS_MUX     = 1,
    PIK_SESSION_CLASS_TUNNEL  = 2,
    PIK_SESSION_CLASS_COUNT   = 3,
} pik_session_class_t;

enum {
    PIK_SESSION_CTRL_QUEUE_CAP = 1u << 13,
    PIK_SESSION_MUX_QUEUE_CAP  = 1u << 20,
    PIK_SESSION_TUN_QUEUE_CAP  = 1u << 17,
};

void pik_session_init(void);
bool pik_session_tick(int64_t now);
int64_t pik_session_deadline(void);
void pik_session_fail(void);
void pik_session_cleanup(void);
bool pik_session_up(void);

/* Queue a sequenced frame for transmission. Failure means the session has
 * been failed (service queue overflow is fail-closed, like the link ring). */
bool pik_session_enqueue(pik_session_class_t cls, uint8_t type, uint8_t ch,
                         const uint8_t *payload, size_t plen);
/* True if a frame of plen can be queued without overflowing the class. */
bool pik_session_can_queue(pik_session_class_t cls, size_t plen);
/* Bytes currently backlogged in the class queue (records + payloads). */
uint32_t pik_session_backlog(pik_session_class_t cls);

/* The underlying link; control and transports use its liveness and byte I/O. */
pik_link_t *pik_session_link(void);
