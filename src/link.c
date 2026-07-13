// src/link.c — shared sequenced serial link used by the mux and control links

#include "link.h"
#include "nanocobs/cobs.h"
#include "fd.h"
#include "frame.h"
#include "logging.h"
#include "tty.h"
#include "util.h"
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>

#define LOGL(lk, ...) pik_log((lk)->cfg.name, __VA_ARGS__)

/* Scratch for one frame; the daemon is single-threaded, so one worst-case
 * sized pair serves every link instance. */
#define SCRATCH_DEC_MAX (8u + PIK_LINK_MAX_PAYLOAD + 4u)
static uint8_t s_dec[SCRATCH_DEC_MAX];
static uint8_t s_enc[COBS_ENCODE_MAX(SCRATCH_DEC_MAX) + 1];

static uint32_t tx_avail(const pik_link_t *lk) { return lk->tx_tail - lk->tx_head; }
static uint32_t tx_space(const pik_link_t *lk) { return lk->cfg.tx_cap - tx_avail(lk); }

uint32_t pik_link_tx_avail(const pik_link_t *lk) { return tx_avail(lk); }

bool pik_link_can_queue(const pik_link_t *lk, size_t plen) {
    if (plen > lk->cfg.max_payload) return false;
    return tx_space(lk) >= COBS_ENCODE_MAX(lk->header_len + plen + 4);
}

bool pik_link_owns_event(const pik_link_t *lk, const void *ptr) {
    return ptr == (const void *)lk;
}

static void refill_tokens(pik_link_t *lk, int64_t now) {
    if (!lk->cfg.tx_rate_bps) return;
    if (!lk->tx_token_ms) lk->tx_token_ms = now;
    int64_t elapsed = now - lk->tx_token_ms;
    if (elapsed <= 0) return;
    uint64_t add = (uint64_t)elapsed * lk->cfg.tx_rate_bps / 1000u;
    if (!add) return;
    uint64_t tokens = (uint64_t)lk->tx_tokens + add;
    lk->tx_tokens = tokens > lk->cfg.tx_burst ? lk->cfg.tx_burst : (uint32_t)tokens;
    lk->tx_token_ms = now;
}

static void update_epoll(pik_link_t *lk) {
    if (lk->fd < 0) return;
    uint32_t want = EPOLLIN;
    if (tx_avail(lk) && (!lk->cfg.tx_rate_bps || lk->tx_tokens))
        want |= EPOLLOUT;
    if (want == lk->epev) return;
    lk->epev = want;
    pik_epoll_set(lk->epfd, lk->fd, want, lk);
}

void pik_link_fail(pik_link_t *lk) {
    lk->failed = true;
    if (lk->fd < 0) return;
    LOGL(lk, "link stats: rx_frames=%llu tx_frames=%llu rx_reads=%llu rx_bytes=%llu "
         "tx_writes=%llu tx_bytes=%llu rx_seq=%u tx_seq=%u",
         (unsigned long long)lk->rx_frames, (unsigned long long)lk->tx_frames,
         (unsigned long long)lk->rx_reads, (unsigned long long)lk->rx_bytes,
         (unsigned long long)lk->tx_writes, (unsigned long long)lk->tx_bytes,
         lk->rx_seq, lk->tx_seq);
    pik_epoll_del(lk->epfd, lk->fd);
    close(lk->fd);
    lk->fd = -1;
    lk->epev = 0;
    if (lk->cfg.on_down)
        lk->cfg.on_down(lk->cfg.ctx);
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
 * They carry the session they are healing (our RX session, i.e. the peer's TX
 * session) so the peer can verify them against its own TX session even when
 * it has never received a sequenced frame from us. */
static bool enqueue_opt(pik_link_t *lk, uint8_t type, uint8_t aux,
                        const uint8_t *payload, size_t plen, bool sequenced) {
    if (lk->fd < 0 || lk->failed) return false;
    if (plen > lk->cfg.max_payload) {
        LOGL(lk, "oversized frame type=0x%02x aux=%u plen=%zu", type, aux, plen);
        pik_link_fail(lk);
        return false;
    }

    uint8_t header[8];
    size_t hl = lk->header_len;
    size_t aux_off = lk->cfg.has_aux ? 1u : 0u;
    header[0] = type;
    if (lk->cfg.has_aux) header[1] = aux;
    pik_put_u32le(header + 1 + aux_off, sequenced ? lk->tx_session : lk->rx_session);
    header[hl - 2] = (uint8_t)lk->tx_seq;
    header[hl - 1] = (uint8_t)(lk->tx_seq >> 8);

    size_t enc_len = 0;
    if (pik_frame_encode(header, hl, payload, plen,
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
    update_epoll(lk);
    return true;
}

bool pik_link_enqueue(pik_link_t *lk, uint8_t type, uint8_t aux,
                      const uint8_t *payload, size_t plen) {
    return enqueue_opt(lk, type, aux, payload, plen, true);
}

static void send_nak(pik_link_t *lk, int64_t now) {
    uint8_t p[2] = { (uint8_t)lk->rx_seq, (uint8_t)(lk->rx_seq >> 8) };
    enqueue_opt(lk, lk->cfg.nak_type, 0, p, sizeof(p), false);
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
    update_epoll(lk);
}

static void handle_frame(pik_link_t *lk, const uint8_t *enc, size_t enc_len) {
    pik_frame_t frame;
    lk->rx_frames++;

    pik_frame_status_t st = pik_frame_decode(enc, enc_len, lk->enc_max,
                                             lk->header_len, s_dec, sizeof(s_dec),
                                             &frame);
    if (st != PIK_FRAME_OK) {
        LOGL(lk, "link failure: %s enc_len=%zu first=0x%02x",
             pik_frame_status_text(st), enc_len, enc_len ? enc[0] : 0);
        pik_log_bad_frame_sample(lk->cfg.name, enc, enc_len);
        pik_link_fail(lk);
        return;
    }

    size_t         aux_off = lk->cfg.has_aux ? 1u : 0u;
    uint8_t        type    = frame.header[0];
    uint8_t        aux     = lk->cfg.has_aux ? frame.header[1] : 0;
    uint32_t       session = pik_get_u32le(frame.header + 1 + aux_off);
    uint16_t       seq     = (uint16_t)frame.header[lk->header_len - 2] |
                             (uint16_t)frame.header[lk->header_len - 1] << 8;
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
        bool first_ok = lk->cfg.first_type ? (type == lk->cfg.first_type)
                                           : (seq == 0);
        if (!first_ok) {
            /* Stale in-flight frames from the peer's previous session are
             * expected physics of an unsynchronized restart: discard them
             * for a bounded window instead of failing the fresh link. */
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

    if (!lk->cfg.on_frame(lk->cfg.ctx, type, aux, payload, plen) || lk->failed)
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

static bool read_available(pik_link_t *lk, int64_t now) {
    while (lk->fd >= 0) {
        size_t cap = lk->cfg.rx_cap - lk->rxbuf_len;
        if (!cap) {
            if (!parse_rx(lk)) return false;
            cap = lk->cfg.rx_cap - lk->rxbuf_len;
            if (!cap) {
                pik_link_fail(lk);
                return false;
            }
        }
        ssize_t n = read(lk->fd, lk->cfg.rxbuf + lk->rxbuf_len, cap);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN) return true;
            LOGL(lk, "link read: %s", strerror(errno));
            pik_link_fail(lk);
            return false;
        }
        if (n == 0) {
            LOGL(lk, "link read: EOF");
            pik_link_fail(lk);
            return false;
        }
        lk->last_rx_ms = now;
        lk->rx_reads++;
        lk->rx_bytes += (uint64_t)n;
        lk->rxbuf_len += (size_t)n;
        if (!parse_rx(lk)) return false;
    }
    return false;
}

static bool drain_tx(pik_link_t *lk, int64_t now) {
    refill_tokens(lk, now);
    uint32_t budget = UINT32_MAX;
    if (lk->cfg.tx_rate_bps) {
        budget = lk->cfg.tx_write_budget;
        if (budget > lk->tx_tokens) budget = lk->tx_tokens;
    }
    while (tx_avail(lk) && budget) {
        uint32_t off    = lk->tx_head & (lk->cfg.tx_cap - 1u);
        uint32_t contig = lk->cfg.tx_cap - off;
        uint32_t n      = tx_avail(lk);
        if (n > contig) n = contig;
        if (n > budget) n = budget;
        ssize_t w = write(lk->fd, lk->cfg.txbuf + off, n);
        if (w <= 0) {
            if (w < 0 && (errno == EAGAIN || errno == EINTR)) break;
            LOGL(lk, "link write: %s", w == 0 ? "EOF" : strerror(errno));
            pik_link_fail(lk);
            return false;
        }
        lk->tx_head += (uint32_t)w;
        lk->last_tx_ms = now;
        lk->tx_writes++;
        lk->tx_bytes += (uint64_t)w;
        budget -= (uint32_t)w;
        if (lk->cfg.tx_rate_bps)
            lk->tx_tokens -= (uint32_t)w;
    }
    update_epoll(lk);
    return true;
}

void pik_link_init(pik_link_t *lk, const pik_link_cfg_t *cfg, int epfd) {
    memset(lk, 0, sizeof(*lk));
    lk->cfg = *cfg;
    lk->epfd = epfd;
    lk->fd = -1;
    lk->header_len = cfg->has_aux ? 8u : 7u;
    lk->enc_max = COBS_ENCODE_MAX(lk->header_len + cfg->max_payload + 4) + 1;
}

bool pik_link_open(pik_link_t *lk, const char *dev, int64_t now) {
    lk->now_ms = now;
    int fd = open(dev, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        LOGL(lk, "link open %s: %s", dev, strerror(errno));
        lk->failed = true;
        return false;
    }
    if (tty_set_byte_raw(fd) < 0) {
        LOGL(lk, "termios setup failed on %s: %s", dev, strerror(errno));
        close(fd);
        lk->failed = true;
        return false;
    }
    if (tty_flush_io(fd) < 0) {
        LOGL(lk, "termios flush failed on %s: %s", dev, strerror(errno));
        close(fd);
        lk->failed = true;
        return false;
    }

    lk->fd = fd;
    lk->failed = false;
    lk->epev = EPOLLIN;
    lk->tx_head = lk->tx_tail = 0;
    lk->rxbuf_len = 0;
    lk->tx_session = pik_session_id(now);
    lk->rx_session = 0;
    lk->tx_seq = lk->rx_seq = 0;
    lk->hist_first = lk->hist_count = 0;
    lk->hist_head = lk->hist_tail = 0;
    lk->last_resend_ms = 0;
    lk->gap_since_ms = 0;
    lk->last_nak_ms = 0;
    lk->gap_discards = lk->dup_discards = 0;
    lk->opened_ms = now;
    lk->stale_discards = 0;
    lk->tx_tokens = lk->cfg.tx_burst;
    lk->tx_token_ms = now;
    lk->last_rx_ms = lk->last_tx_ms = now;
    pik_epoll_set(lk->epfd, fd, EPOLLIN, lk);
    LOGL(lk, "link opened: %s", dev);
    return true;
}

bool pik_link_dispatch(pik_link_t *lk, uint32_t events, int64_t now) {
    lk->now_ms = now;
    if (events & (EPOLLERR | EPOLLHUP)) {
        pik_link_fail(lk);
        return false;
    }
    if (events & EPOLLIN)
        read_available(lk, now);
    if (!lk->failed && (events & EPOLLOUT))
        drain_tx(lk, now);
    return !lk->failed;
}

bool pik_link_tick(pik_link_t *lk, int64_t now) {
    lk->now_ms = now;
    if (lk->fd < 0 || lk->failed) return false;
    if (tx_avail(lk))
        drain_tx(lk, now);
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
    if (lk->fd < 0) return INT64_MAX;
    int64_t dl = INT64_MAX;
    if (lk->gap_since_ms) {
        int64_t a = lk->gap_since_ms + PIK_LINK_GAP_BUDGET_MS;
        int64_t b = lk->last_nak_ms + PIK_LINK_NAK_RETRY_MS;
        dl = a < b ? a : b;
    }
    /* paced and waiting on tokens: wake to drain as they refill */
    if (lk->cfg.tx_rate_bps && tx_avail(lk) && !lk->tx_tokens) {
        int64_t t = lk->now_ms + 1;
        if (t < dl) dl = t;
    }
    return dl;
}

void pik_link_cleanup(pik_link_t *lk) {
    if (lk->fd >= 0) {
        pik_epoll_del(lk->epfd, lk->fd);
        close(lk->fd);
        lk->fd = -1;
    }
    lk->failed = false;
    lk->epev = 0;
    lk->tx_head = lk->tx_tail = 0;
    lk->rxbuf_len = 0;
    lk->tx_session = lk->rx_session = 0;
    lk->tx_seq = lk->rx_seq = 0;
    lk->gap_since_ms = 0;
    lk->last_nak_ms = 0;
}
