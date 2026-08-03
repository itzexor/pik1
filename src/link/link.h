#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "frame.h"

/* Shared sequenced link core: COBS+CRC framing, session/sequence tracking,
 * bring-up grace for stale peer frames, and bounded link-layer retransmission
 * (FAILURE_MODEL.md, "Documented Exceptions").
 *
 * Wire header layout: [type:1][aux:1][session_le:4][seq_le:2].
 * The aux byte carries the service channel id (pik_proto.h). */

#define PIK_LINK_MAX_PAYLOAD    PIK_WIRE_MAX_PAYLOAD
#define PIK_LINK_HEADER_LEN     PIK_WIRE_HEADER_LEN

#define PIK_LINK_NAK_RETRY_MS   100     /* re-send NAK while a gap persists */
#define PIK_LINK_GAP_BUDGET_MS  500     /* unhealed gap fails the link */
#define PIK_LINK_STALE_GRACE_MS 2000    /* discard stale frames after link open */

typedef struct {
    uint32_t off;   /* free-running byte offset into hist ring */
    uint16_t len;
} pik_link_hist_ent_t;

typedef struct {
    const char *name;       /* log component tag */
    uint8_t     nak_type;   /* link-control NAK frame type */
    uint8_t     first_type; /* first sequenced frame must be this type */

    /* caller-owned buffers; ring capacities must be powers of two */
    uint8_t             *txbuf;     uint32_t tx_cap;
    uint8_t             *rxbuf;     size_t   rx_cap;
    uint8_t             *hist;      uint32_t hist_cap;
    pik_link_hist_ent_t *hist_ent;  uint16_t hist_slots;

    /* Deliver a synchronized sequenced frame; return false to fail the link. */
    bool (*on_frame)(uint8_t type, uint8_t aux,
                     const uint8_t *payload, size_t plen);
    /* Optional transport wakeup after encoded bytes enter the TX ring. */
    void (*on_tx_ready)(void);
    /* Called once when an open link fails. */
    void (*on_down)(void);
} pik_link_cfg_t;

typedef struct {
    pik_link_cfg_t cfg;
    bool     active;        /* transport has begun a session */
    bool     failed;
    size_t   enc_max;       /* per-frame encoded ceiling incl. delimiter */

    uint32_t tx_head, tx_tail;
    size_t   rxbuf_len;

    uint32_t tx_session, rx_session;
    uint16_t tx_seq, rx_seq;

    /* retransmit history of sent frames (encoded bytes + per-seq index) */
    uint16_t hist_first;    /* seq of oldest frame still held */
    uint16_t hist_count;
    uint32_t hist_head, hist_tail;
    int64_t  last_resend_ms;

    /* receiver gap recovery */
    int64_t  gap_since_ms;  /* 0 = stream in sync */
    int64_t  last_nak_ms;
    uint32_t gap_discards;
    uint32_t dup_discards;

    /* bring-up grace for stale frames from the peer's previous session */
    int64_t  opened_ms;
    uint32_t stale_discards;

    int64_t  last_rx_ms, last_tx_ms;
    int64_t  now_ms;        /* updated on entry to open/dispatch/tick */
} pik_link_t;

void pik_link_init(pik_link_t *lk, const pik_link_cfg_t *cfg);
void pik_link_begin(pik_link_t *lk, int64_t now);
void pik_link_fail(pik_link_t *lk);
void pik_link_cleanup(pik_link_t *lk);

bool pik_link_is_open(const pik_link_t *lk);
/* Transport-facing byte interface: backends feed received bytes here and drain
 * encoded bytes from the TX ring with peek/consume. */
bool pik_link_feed(pik_link_t *lk, const uint8_t *buf, size_t len, int64_t now);
const uint8_t *pik_link_tx_peek(pik_link_t *lk, uint32_t *len);
void pik_link_tx_consume(pik_link_t *lk, uint32_t len, int64_t now);

bool pik_link_tick(pik_link_t *lk, int64_t now);
int64_t pik_link_deadline(const pik_link_t *lk);

bool pik_link_enqueue(pik_link_t *lk, uint8_t type, uint8_t aux,
                      const uint8_t *payload, size_t plen);
uint32_t pik_link_tx_avail(const pik_link_t *lk);
