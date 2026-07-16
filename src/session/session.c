// Single shared link: service router and priority TX scheduler

#include "session.h"
#include "pik_proto.h"
#include "control.h"
#include "serialmux.h"
#include "tunnel.h"
#include "logging.h"

#include <string.h>

#define LOG(...) pik_log("link", __VA_ARGS__)

// ── sizing ────────────────────────────────────────────────────────────────────
#define LINK_TX_CAP   (1u << 19)    /* must exceed HIST_CAP retransmit bursts */
#define LINK_RX_CAP   (1u << 16)
#define HIST_CAP      (1u << 18)
#define HIST_SLOTS    512u

/* Admission target: keep the wire FIFO to one encoded frame at a time. Class
 * priority only applies before frames enter the wire FIFO. */
#define ADMIT_TARGET  1u

#define REC_HDR_LEN   4u            /* [type:1][ch:1][len_le:2] */

typedef struct {
    uint8_t *buf;
    uint32_t cap;                   /* power of two */
    uint32_t head, tail;
} classq_t;

static pik_link_t g_link;

static uint8_t s_link_tx[LINK_TX_CAP];
static uint8_t s_link_rx[LINK_RX_CAP];
static uint8_t s_link_hist[HIST_CAP];
static pik_link_hist_ent_t s_link_hist_ent[HIST_SLOTS];

static uint8_t s_ctrl_q[PIK_SESSION_CTRL_QUEUE_CAP];
static uint8_t s_mux_q[PIK_SESSION_MUX_QUEUE_CAP];
static uint8_t s_tun_q[PIK_SESSION_TUN_QUEUE_CAP];

static classq_t g_q[PIK_SESSION_CLASS_COUNT] = {
    { s_ctrl_q, PIK_SESSION_CTRL_QUEUE_CAP, 0, 0 },
    { s_mux_q,  PIK_SESSION_MUX_QUEUE_CAP,  0, 0 },
    { s_tun_q,  PIK_SESSION_TUN_QUEUE_CAP,  0, 0 },
};

static uint8_t s_pop_payload[PIK_LINK_MAX_PAYLOAD];

// ── class queues ──────────────────────────────────────────────────────────────
static uint32_t q_avail(const classq_t *q) { return q->tail - q->head; }
static uint32_t q_space(const classq_t *q) { return q->cap - q_avail(q); }

static void queues_reset(void) {
    for (int c = 0; c < PIK_SESSION_CLASS_COUNT; c++)
        g_q[c].head = g_q[c].tail = 0;
}

static void q_push_bytes(classq_t *q, const uint8_t *src, size_t len) {
    for (size_t i = 0; i < len; i++)
        q->buf[q->tail++ & (q->cap - 1u)] = src[i];
}

static void q_pop_bytes(classq_t *q, uint8_t *dst, size_t len) {
    for (size_t i = 0; i < len; i++)
        dst[i] = q->buf[q->head++ & (q->cap - 1u)];
}

/* Admit queued frames onto the wire FIFO in strict class priority while it
 * is below the admission target. */
static void sched_drain(void) {
    while (pik_link_is_open(&g_link) &&
           pik_link_tx_avail(&g_link) < ADMIT_TARGET) {
        classq_t *q = NULL;
        for (int c = 0; c < PIK_SESSION_CLASS_COUNT; c++) {
            if (q_avail(&g_q[c])) { q = &g_q[c]; break; }
        }
        if (!q) return;

        uint8_t rec[REC_HDR_LEN];
        q_pop_bytes(q, rec, sizeof(rec));
        uint8_t type = rec[0];
        uint8_t ch = rec[1];
        size_t plen = (size_t)rec[2] | (size_t)rec[3] << 8;
        q_pop_bytes(q, s_pop_payload, plen);
        if (!pik_link_enqueue(&g_link, type, ch, s_pop_payload, plen))
            return;
    }
}

bool pik_session_enqueue(pik_session_class_t cls, uint8_t type, uint8_t ch,
                         const uint8_t *payload, size_t plen) {
    classq_t *q = &g_q[cls];
    if (!pik_link_is_open(&g_link)) return false;
    if (plen > PIK_LINK_MAX_PAYLOAD ||
        q_space(q) < REC_HDR_LEN + plen) {
        LOG("class %d queue full, closing before dropping frame type=0x%02x ch=%u plen=%zu",
            cls, type, ch, plen);
        pik_session_fail();
        return false;
    }
    uint8_t rec[REC_HDR_LEN] = { type, ch, (uint8_t)plen, (uint8_t)(plen >> 8) };
    q_push_bytes(q, rec, sizeof(rec));
    if (plen) q_push_bytes(q, payload, plen);
    sched_drain();
    return !g_link.failed;
}

bool pik_session_can_queue(pik_session_class_t cls, size_t plen) {
    if (plen > PIK_LINK_MAX_PAYLOAD) return false;
    return q_space(&g_q[cls]) >= REC_HDR_LEN + plen;
}

uint32_t pik_session_backlog(pik_session_class_t cls) {
    return q_avail(&g_q[cls]);
}

// ── inbound routing ───────────────────────────────────────────────────────────
static bool session_on_frame(uint8_t type, uint8_t ch,
                             const uint8_t *payload, size_t plen) {
    switch (type & 0xf0u) {
    case 0x00u:
        if (!pik_mux_wire_valid(ch)) {
            LOG("link failure: mux frame on bad channel type=0x%02x ch=%u", type, ch);
            return false;
        }
        return serialmux_on_frame(type, ch, payload, plen);
    case 0x10u:
        if (ch != PIK_CH_CONTROL) {
            LOG("link failure: control frame on bad channel type=0x%02x ch=%u", type, ch);
            return false;
        }
        return pik_control_on_frame(type, payload, plen);
    case 0x20u:
        if (ch != PIK_CH_TUNNEL) {
            LOG("link failure: tunnel frame on bad channel type=0x%02x ch=%u", type, ch);
            return false;
        }
        return tunnel_on_frame(type, payload, plen);
    default:
        LOG("link failure: unknown frame type=0x%02x ch=%u len=%zu", type, ch, plen);
        return false;
    }
}

static void session_on_down(void) {
    tunnel_on_link_down();
    serialmux_on_link_down();
    pik_control_on_link_down();
}

// ── lifecycle ─────────────────────────────────────────────────────────────────
void pik_session_init(void) {
    pik_link_cfg_t cfg = {
        .name        = "link",
        .nak_type    = PIK_FRAME_NAK,
        .first_type  = PIK_FRAME_CTRL_HELLO,
        .txbuf       = s_link_tx,       .tx_cap     = LINK_TX_CAP,
        .rxbuf       = s_link_rx,       .rx_cap     = sizeof(s_link_rx),
        .hist        = s_link_hist,     .hist_cap   = HIST_CAP,
        .hist_ent    = s_link_hist_ent, .hist_slots = HIST_SLOTS,
        .on_frame    = session_on_frame,
        .on_down     = session_on_down,
    };
    pik_link_init(&g_link, &cfg);
}

bool pik_session_tick(int64_t now) {
    if (!pik_link_tick(&g_link, now))
        return false;
    sched_drain();
    return !g_link.failed;
}

int64_t pik_session_deadline(void) {
    return pik_link_deadline(&g_link);
}

void pik_session_fail(void) {
    pik_link_fail(&g_link);
}

void pik_session_cleanup(void) {
    pik_link_cleanup(&g_link);
    queues_reset();
}

bool pik_session_up(void) {
    return pik_link_is_open(&g_link);
}

pik_link_t *pik_session_link(void) {
    return &g_link;
}
