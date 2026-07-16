// Shared sequenced link core used by the single session

#include "link.h"
#include "nanocobs/cobs.h"
#include "frame.h"
#include "logging.h"
#include "util.h"
#include <string.h>

#define LOGL(lk, ...) pik_log((lk)->cfg.name, __VA_ARGS__)

/* Scratch for one frame; the daemon is single-threaded, so one worst-case
 * sized pair serves every link instance. */
#define SCRATCH_DEC_MAX (8u + PIK_LINK_MAX_PAYLOAD + 4u)
static uint8_t s_dec[SCRATCH_DEC_MAX];
static uint8_t s_enc[COBS_ENCODE_MAX(SCRATCH_DEC_MAX) + 1];

static uint32_t tx_avail(const pik_link_t *lk) { return lk->tx_tail - lk->tx_head; }
static uint32_t tx_space(const pik_link_t *lk) { return lk->cfg.tx_cap - tx_avail(lk); }

uint32_t pik_link_tx_avail(const pik_link_t *lk) { return tx_avail(lk); }

const uint8_t *pik_link_tx_peek(pik_link_t *lk, uint32_t *len) {
    *len = 0;
    if (!lk->active || lk->failed) return NULL;
    if (!tx_avail(lk)) return NULL;

    uint32_t off = lk->tx_head & (lk->cfg.tx_cap - 1u);
    uint32_t n = tx_avail(lk);
    uint32_t contig = lk->cfg.tx_cap - off;
    if (n > contig) n = contig;
    *len = n;
    return lk->cfg.txbuf + off;
}

void pik_link_tx_consume(pik_link_t *lk, uint32_t len, int64_t now) {
    if (!len) return;
    lk->tx_head += len;
    lk->last_tx_ms = now;
    lk->tx_writes++;
    lk->tx_bytes += (uint64_t)len;
}

void pik_link_fail(pik_link_t *lk) {
    bool was_active = lk->active;
    lk->failed = true;
    if (!was_active) return;
    if (!lk->quiet)
        LOGL(lk, "link stats: rx_frames=%llu tx_frames=%llu rx_reads=%llu rx_bytes=%llu "
             "tx_writes=%llu tx_bytes=%llu rx_seq=%u tx_seq=%u",
             (unsigned long long)lk->rx_frames, (unsigned long long)lk->tx_frames,
             (unsigned long long)lk->rx_reads, (unsigned long long)lk->rx_bytes,
             (unsigned long long)lk->tx_writes, (unsigned long long)lk->tx_bytes,
             lk->rx_seq, lk->tx_seq);
    lk->active = false;
    if (lk->cfg.on_down)
        lk->cfg.on_down();
}

static void hist_store(pik_link_t *lk, uint16_t seq, const uint8_t *enc, size_t enc_len) {
    while (lk->hist_count &&
           (lk->hist_count == lk->cfg.hist_slots ||
            lk->cfg.hist_cap - (lk->hist_tail - lk->hist_head) < (uint32_t)enc_len)) {
        lk->hist_head += lk->cfg.hist_ent[lk->hist_first % lk->cfg.hist_slots].len;
        lk->hist_first++;
        lk->hist_count--;
    }
    if (!lk->hist_count) {
        lk->hist_first = seq;
        lk->hist_head = lk->hist_tail;
    }
    pik_link_hist_ent_t *e = &lk->cfg.hist_ent[seq % lk->cfg.hist_slots];
    e->off = lk->hist_tail;
    e->len = (uint16_t)enc_len;
    for (size_t i = 0; i < enc_len; i++)
        lk->cfg.hist[(lk->hist_tail + i) & (lk->cfg.hist_cap - 1u)] = enc[i];
    lk->hist_tail += (uint32_t)enc_len;
    lk->hist_count++;
}

/* sequenced=false is for link-control frames (NAK): they consume no sequence
 * number and are never retransmitted, so a peer can process them mid-gap.
 * They carry the session they are healing (the peer's TX session) so the peer
 * can verify them against its own TX session even when it has never received
 * a sequenced frame from us. */
static bool enqueue_opt(pik_link_t *lk, uint8_t type, uint8_t aux,
                        const uint8_t *payload, size_t plen,
                        bool sequenced, uint32_t session) {
    if (!lk->active || lk->failed) return false;
    if (plen > PIK_LINK_MAX_PAYLOAD) {
        LOGL(lk, "oversized frame type=0x%02x aux=%u plen=%zu", type, aux, plen);
        pik_link_fail(lk);
        return false;
    }

    uint8_t header[PIK_LINK_HEADER_LEN];
    header[0] = type;
    header[1] = aux;
    pik_put_u32le(header + 2, session);
    header[6] = (uint8_t)lk->tx_seq;
    header[7] = (uint8_t)(lk->tx_seq >> 8);

    size_t enc_len = 0;
    if (pik_frame_encode(header, payload, plen,
                         s_dec, sizeof(s_dec), s_enc, sizeof(s_enc),
                         &enc_len) != PIK_FRAME_OK) {
        LOGL(lk, "encode failed type=0x%02x aux=%u", type, aux);
        pik_link_fail(lk);
        return false;
    }
    if (tx_space(lk) < enc_len) {
        LOGL(lk, "link TX ring full, closing before dropping frame type=0x%02x aux=%u",
             type, aux);
        pik_link_fail(lk);
        return false;
    }
    for (size_t i = 0; i < enc_len; i++)
        lk->cfg.txbuf[lk->tx_tail++ & (lk->cfg.tx_cap - 1u)] = s_enc[i];
    if (sequenced) {
        hist_store(lk, lk->tx_seq, s_enc, enc_len);
        lk->tx_seq++;
    }
    lk->tx_frames++;
    if (lk->cfg.on_tx_ready)
        lk->cfg.on_tx_ready();
    return true;
}

bool pik_link_enqueue(pik_link_t *lk, uint8_t type, uint8_t aux,
                      const uint8_t *payload, size_t plen) {
    return enqueue_opt(lk, type, aux, payload, plen, true, lk->tx_session);
}

static void send_nak(pik_link_t *lk, int64_t now) {
    uint8_t p[2] = { (uint8_t)lk->rx_seq, (uint8_t)(lk->rx_seq >> 8) };
    enqueue_opt(lk, lk->cfg.nak_type, 0, p, sizeof(p), false, lk->rx_session);
    lk->last_nak_ms = now;
}

static void handle_nak(pik_link_t *lk, const uint8_t *p) {
    uint16_t expected = (uint16_t)p[0] | (uint16_t)p[1] << 8;
    uint16_t outstanding = (uint16_t)(lk->tx_seq - expected);
    if (outstanding == 0) return; /* peer caught up while the NAK was in flight */
    if (outstanding > lk->hist_count) {
        LOGL(lk, "link failure: NAK beyond retransmit window expected=%u tx_seq=%u window=%u",
             expected, lk->tx_seq, lk->hist_count);
        pik_link_fail(lk);
        return;
    }
    if (lk->last_resend_ms && lk->now_ms - lk->last_resend_ms < PIK_LINK_NAK_RETRY_MS / 2)
        return; /* duplicate NAK burst; resend already queued */

    LOGL(lk, "retransmit: seq=%u..%u frames=%u (peer NAK)",
         expected, (uint16_t)(lk->tx_seq - 1u), outstanding);
    for (uint16_t s = expected; s != lk->tx_seq; s++) {
        const pik_link_hist_ent_t *e = &lk->cfg.hist_ent[s % lk->cfg.hist_slots];
        if (tx_space(lk) < e->len) {
            LOGL(lk, "link TX ring full during retransmit");
            pik_link_fail(lk);
            return;
        }
        for (uint16_t i = 0; i < e->len; i++)
            lk->cfg.txbuf[lk->tx_tail++ & (lk->cfg.tx_cap - 1u)] =
                lk->cfg.hist[(e->off + i) & (lk->cfg.hist_cap - 1u)];
    }
    lk->last_resend_ms = lk->now_ms;
    if (lk->cfg.on_tx_ready)
        lk->cfg.on_tx_ready();
}

static void handle_frame(pik_link_t *lk, const uint8_t *enc, size_t enc_len) {
    pik_frame_t frame;
    lk->rx_frames++;

    pik_frame_status_t st = pik_frame_decode(enc, enc_len, lk->enc_max,
                                             s_dec, sizeof(s_dec), &frame);
    if (st != PIK_FRAME_OK) {
        if (lk->rx_session == 0 &&
            lk->now_ms - lk->opened_ms <= PIK_LINK_STALE_GRACE_MS) {
            /* Damaged residue from the peer's previous session is discarded
             * during the stale-frame grace window. */
            if (!lk->stale_discards)
                LOGL(lk, "discarding stale bring-up garbage (%s enc_len=%zu)",
                     pik_frame_status_text(st), enc_len);
            lk->stale_discards++;
            return;
        }
        if (lk->rx_session != 0) {
            /* A damaged frame is treated as a lost frame and recovered through
             * the bounded NAK path. */
            if (!lk->gap_since_ms) {
                lk->gap_since_ms = lk->now_ms;
                lk->gap_discards = 0;
                LOGL(lk, "bad frame (%s enc_len=%zu), requesting retransmit",
                     pik_frame_status_text(st), enc_len);
                pik_log_bad_frame_sample(lk->cfg.name, enc, enc_len);
                send_nak(lk, lk->now_ms);
            }
            lk->gap_discards++;
            return;
        }
        LOGL(lk, "link failure: %s enc_len=%zu first=0x%02x",
             pik_frame_status_text(st), enc_len, enc_len ? enc[0] : 0);
        pik_log_bad_frame_sample(lk->cfg.name, enc, enc_len);
        pik_link_fail(lk);
        return;
    }

    uint8_t        type    = frame.header[0];
    uint8_t        aux     = frame.header[1];
    uint32_t       session = pik_get_u32le(frame.header + 2);
    uint16_t       seq     = (uint16_t)frame.header[6] |
                             (uint16_t)frame.header[7] << 8;
    size_t         plen    = frame.payload_len;
    const uint8_t *payload = frame.payload;

    if (session == 0) {
        LOGL(lk, "link failure: zero session type=0x%02x aux=%u", type, aux);
        pik_link_fail(lk);
        return;
    }

    if (type == lk->cfg.nak_type) {
        /* Link-control: not sequenced, and it carries the session it is
         * healing (our TX session), so it stays verifiable even when the
         * peer has never sent us a sequenced frame. Ignore unless it is
         * provably about our live session. */
        if (session != lk->tx_session) return;
        if (plen != 2) {
            LOGL(lk, "link failure: bad NAK len=%zu", plen);
            pik_link_fail(lk);
            return;
        }
        handle_nak(lk, payload);
        return;
    }

    if (lk->rx_session == 0) {
        bool first_ok = type == lk->cfg.first_type;
        if (!first_ok) {
            /* Stale in-flight frames from the peer's previous session are
             * discarded for a bounded window instead of failing the fresh
             * link. */
            if (lk->now_ms - lk->opened_ms <= PIK_LINK_STALE_GRACE_MS) {
                if (!lk->stale_discards)
                    LOGL(lk, "discarding stale bring-up frames (session=0x%08x seq=%u type=0x%02x aux=%u)",
                         session, seq, type, aux);
                lk->stale_discards++;
                return;
            }
            LOGL(lk, "link failure: stale frames past bring-up grace seq=%u type=0x%02x aux=%u discarded=%u",
                 seq, type, aux, lk->stale_discards);
            pik_link_fail(lk);
            return;
        }
        lk->rx_session = session;
        lk->rx_seq = (uint16_t)(seq + 1);
        if (lk->stale_discards)
            LOGL(lk, "bring-up synchronized, discarded %u stale frames",
                 lk->stale_discards);
    } else if (session != lk->rx_session) {
        LOGL(lk, "link failure: session changed old=0x%08x new=0x%08x type=0x%02x aux=%u",
             lk->rx_session, session, type, aux);
        pik_link_fail(lk);
        return;
    } else {
        int16_t d = (int16_t)(seq - lk->rx_seq);
        if (d < 0) {
            /* duplicate from retransmission overlap; already delivered */
            lk->dup_discards++;
            return;
        }
        if (d > 0) {
            if (!lk->gap_since_ms) {
                lk->gap_since_ms = lk->now_ms;
                lk->gap_discards = 0;
                LOGL(lk, "seq gap session=0x%08x seq=%u expected=%u type=0x%02x aux=%u, requesting retransmit",
                     session, seq, lk->rx_seq, type, aux);
                send_nak(lk, lk->now_ms);
            }
            lk->gap_discards++;
            return;
        }
        lk->rx_seq++;
        if (lk->gap_since_ms) {
            LOGL(lk, "retransmit healed: seq=%u latency=%lldms discarded=%u dups=%u",
                 seq, (long long)(lk->now_ms - lk->gap_since_ms),
                 lk->gap_discards, lk->dup_discards);
            lk->gap_since_ms = 0;
            lk->gap_discards = 0;
            lk->dup_discards = 0;
        }
    }

    if (!lk->cfg.on_frame(type, aux, payload, plen) || lk->failed)
        pik_link_fail(lk);
}

static bool rx_frame_cb(void *ctx, const uint8_t *enc, size_t enc_len) {
    pik_link_t *lk = ctx;
    handle_frame(lk, enc, enc_len);
    return !lk->failed;
}

static bool parse_rx(pik_link_t *lk) {
    pik_frame_status_t st = pik_frame_rx_consume(lk->cfg.rxbuf, &lk->rxbuf_len,
                                                 lk->cfg.rx_cap, rx_frame_cb, lk);
    if (st == PIK_FRAME_CALLBACK_FAILED) return false;
    if (st == PIK_FRAME_RX_OVERFLOW) {
        LOGL(lk, "link RX overflow");
        pik_link_fail(lk);
        return false;
    }
    return true;
}

bool pik_link_feed(pik_link_t *lk, const uint8_t *buf, size_t len, int64_t now) {
    lk->now_ms = now;
    if (!lk->active || lk->failed) return false;
    if (!len) return true;

    lk->last_rx_ms = now;
    lk->rx_reads++;
    lk->rx_bytes += (uint64_t)len;

    while (len && !lk->failed) {
        size_t cap = lk->cfg.rx_cap - lk->rxbuf_len;
        if (!cap) {
            if (!parse_rx(lk)) return false;
            cap = lk->cfg.rx_cap - lk->rxbuf_len;
            if (!cap) {
                pik_link_fail(lk);
                return false;
            }
        }
        size_t n = len < cap ? len : cap;
        memcpy(lk->cfg.rxbuf + lk->rxbuf_len, buf, n);
        lk->rxbuf_len += n;
        buf += n;
        len -= n;
        if (!parse_rx(lk)) return false;
    }
    return !lk->failed;
}

void pik_link_init(pik_link_t *lk, const pik_link_cfg_t *cfg) {
    memset(lk, 0, sizeof(*lk));
    lk->cfg = *cfg;
    lk->active = false;
    lk->enc_max = COBS_ENCODE_MAX(PIK_LINK_HEADER_LEN + PIK_LINK_MAX_PAYLOAD + 4) + 1;
}

void pik_link_begin(pik_link_t *lk, int64_t now) {
    lk->now_ms = now;
    lk->active = true;
    lk->failed = false;
    lk->tx_head = lk->tx_tail = 0;
    lk->rxbuf_len = 0;
    lk->tx_session = pik_session_id(now);
    lk->rx_session = 0;
    lk->tx_seq = lk->rx_seq = 0;
    lk->rx_frames = lk->tx_frames = 0;
    lk->rx_reads = lk->rx_bytes = 0;
    lk->tx_writes = lk->tx_bytes = 0;
    lk->hist_first = lk->hist_count = 0;
    lk->hist_head = lk->hist_tail = 0;
    lk->last_resend_ms = 0;
    lk->gap_since_ms = 0;
    lk->last_nak_ms = 0;
    lk->gap_discards = lk->dup_discards = 0;
    lk->opened_ms = now;
    lk->stale_discards = 0;
    lk->last_rx_ms = lk->last_tx_ms = now;
}

bool pik_link_is_open(const pik_link_t *lk) {
    return lk->active && !lk->failed;
}

bool pik_link_tick(pik_link_t *lk, int64_t now) {
    lk->now_ms = now;
    if (!lk->active || lk->failed) return false;
    if (!lk->failed && lk->gap_since_ms) {
        if (now - lk->gap_since_ms > PIK_LINK_GAP_BUDGET_MS) {
            LOGL(lk, "link failure: seq gap not healed expected=%u after %dms discarded=%u",
                 lk->rx_seq, PIK_LINK_GAP_BUDGET_MS, lk->gap_discards);
            pik_link_fail(lk);
            return false;
        }
        if (now - lk->last_nak_ms >= PIK_LINK_NAK_RETRY_MS)
            send_nak(lk, now);
    }
    return !lk->failed;
}

int64_t pik_link_deadline(const pik_link_t *lk) {
    if (!lk->active) return INT64_MAX;
    int64_t dl = INT64_MAX;
    if (lk->gap_since_ms) {
        int64_t a = lk->gap_since_ms + PIK_LINK_GAP_BUDGET_MS;
        int64_t b = lk->last_nak_ms + PIK_LINK_NAK_RETRY_MS;
        dl = a < b ? a : b;
    }
    return dl;
}

void pik_link_cleanup(pik_link_t *lk) {
    lk->active = false;
    lk->failed = false;
    lk->quiet = false;
    lk->tx_head = lk->tx_tail = 0;
    lk->rxbuf_len = 0;
    lk->tx_session = lk->rx_session = 0;
    lk->tx_seq = lk->rx_seq = 0;
    lk->gap_since_ms = 0;
    lk->last_nak_ms = 0;
}
