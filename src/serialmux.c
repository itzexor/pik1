// src/serialmux.c — COBS serial multiplexer, library interface

#include "serialmux.h"
#include "nanocobs/cobs.h"
#include "fd.h"
#include "frame.h"
#include "logging.h"
#include "tty.h"
#include "util.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pty.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>

// ── frame types ───────────────────────────────────────────────────────────────
#define F_DATA   0x01u
#define F_FLUSH  0x02u
#define F_READY  0x03u

// ── sizing ────────────────────────────────────────────────────────────────────
#define MAX_PAYLOAD     4096

#define FRAME_HEADER_LEN 8u
#define FRAME_DEC_MAX   (FRAME_HEADER_LEN + MAX_PAYLOAD + 4)
#define FRAME_ENC_MAX   COBS_ENCODE_MAX(FRAME_DEC_MAX)

#define LINK_RING_CAP   (1u << 20)
#define LINK_RING_MASK  (LINK_RING_CAP - 1u)
#define LINK_HIGH_WATER (LINK_RING_CAP / 2)
#define LINK_LOW_WATER  (LINK_RING_CAP / 4)

#define CHAN_RING_CAP   (1u << 16)
#define CHAN_RING_MASK  (CHAN_RING_CAP - 1u)

#define RESET_SILENCE_MS  5000

// ── types ─────────────────────────────────────────────────────────────────────
typedef enum { MCU_INIT, MCU_ACTIVE, MCU_RESETTING } mcu_state_t;

typedef struct channel {
    ch_type_t   type;
    uint8_t     ch_id;
    int         fd;
    bool        paused;
    uint32_t    epev;

    uint8_t     txbuf[CHAN_RING_CAP];
    uint32_t    tx_head, tx_tail;

    // MCU
    char        dev[128];
    int         baud;
    mcu_state_t mcu_state;
    int64_t     last_byte_ms;

    // PTY
    char        pty_path[128];
    int         slave_fd;
} channel_t;

typedef struct {
    int         fd;
    bool        up;
    bool        paused;
    uint32_t    epev;

    uint8_t     txbuf[LINK_RING_CAP];
    uint32_t    tx_head, tx_tail;

    uint8_t     rxbuf[FRAME_ENC_MAX + 4];
    size_t      rxbuf_len;

    uint32_t    tx_session;
    uint32_t    rx_session;
    uint16_t    tx_seq;
    uint16_t    rx_seq;

} link_t;

// ── globals ───────────────────────────────────────────────────────────────────
static link_t    g_link;
static channel_t g_chans[MAX_CHANNELS];
static int       g_n_chans;
static int       g_epfd = -1;
static bool      g_session_failed;

// ── logging ───────────────────────────────────────────────────────────────────
#define LOG(...)  pik_log("mux", __VA_ARGS__)
#define DIE(...)  pik_die("mux", __VA_ARGS__)

// ── channel ring helpers ──────────────────────────────────────────────────────
static uint32_t chan_avail(const channel_t *c) { return c->tx_tail - c->tx_head; }
static uint32_t chan_space(const channel_t *c) { return CHAN_RING_CAP - chan_avail(c); }

static void chan_push(channel_t *c, const uint8_t *src, size_t len) {
    for (size_t i = 0; i < len; i++)
        c->txbuf[c->tx_tail++ & CHAN_RING_MASK] = src[i];
}

static bool chan_drain(channel_t *c) {
    while (chan_avail(c)) {
        uint32_t off    = c->tx_head & CHAN_RING_MASK;
        uint32_t contig = CHAN_RING_CAP - off;
        uint32_t avail  = chan_avail(c);
        size_t   n      = avail < contig ? avail : contig;
        ssize_t  w      = write(c->fd, c->txbuf + off, n);
        if (w <= 0) {
            if (w < 0 && errno != EAGAIN && errno != EINTR) {
                LOG("ch%u write: %s", c->ch_id, strerror(errno));
                return false;
            }
            break;
        }
        c->tx_head += (uint32_t)w;
    }
    return true;
}

static void chan_epoll_update(channel_t *c) {
    if (c->fd < 0) return;
    uint32_t want = (!c->paused ? EPOLLIN : 0u) | (chan_avail(c) ? EPOLLOUT : 0u);
    if (want == c->epev) return;
    c->epev = want;
    if (want)
        pik_epoll_set(g_epfd, c->fd, want, c);
    else
        pik_epoll_del(g_epfd, c->fd);
}

static void chan_pause(channel_t *c)  { if (!c->paused) { c->paused = true;  chan_epoll_update(c); } }
static void chan_resume(channel_t *c) { if ( c->paused) { c->paused = false; chan_epoll_update(c); } }

// ── link ring helpers ─────────────────────────────────────────────────────────
static uint32_t lk_avail(const link_t *lk) { return lk->tx_tail - lk->tx_head; }
static uint32_t lk_space(const link_t *lk) { return LINK_RING_CAP - lk_avail(lk); }

static bool lk_push(link_t *lk, const uint8_t *src, size_t len) {
    if (lk_space(lk) < (uint32_t)len) return false;
    for (size_t i = 0; i < len; i++)
        lk->txbuf[lk->tx_tail++ & LINK_RING_MASK] = src[i];
    return true;
}

static bool lk_drain(link_t *lk) {
    bool wrote = false;
    while (lk_avail(lk)) {
        uint32_t off    = lk->tx_head & LINK_RING_MASK;
        uint32_t contig = LINK_RING_CAP - off;
        uint32_t avail  = lk_avail(lk);
        size_t   n      = avail < contig ? avail : contig;
        ssize_t  w      = write(lk->fd, lk->txbuf + off, n);
        if (w <= 0) {
            if (w < 0 && errno != EAGAIN && errno != EINTR)
                LOG("link write: %s", strerror(errno));
            break;
        }
        lk->tx_head += (uint32_t)w;
        wrote = true;
    }
    uint32_t want = EPOLLIN | (lk_avail(lk) ? EPOLLOUT : 0u);
    if (want != lk->epev && lk->fd >= 0) {
        lk->epev = want;
        pik_epoll_set(g_epfd, lk->fd, want, lk);
    }
    return wrote;
}

// ── frame I/O ────────────────────────────────────────────────────────────────
static void dispatch_frame(link_t *lk, const uint8_t *enc, size_t enc_len);
static bool dispatch_rx_frame(void *ctx, const uint8_t *enc, size_t enc_len);
static void link_close(link_t *lk);

static void link_fail_frame(link_t *lk, const char *reason,
                            const uint8_t *enc, size_t enc_len) {
    LOG("link failure: %s enc_len=%zu first=0x%02x", reason, enc_len,
        enc_len ? enc[0] : 0);
    pik_log_bad_frame_sample("mux", enc, enc_len);
    link_close(lk);
}

static bool link_can_queue_frame(const link_t *lk, size_t plen) {
    if (plen > MAX_PAYLOAD) return false;
    return lk_space(lk) >= COBS_ENCODE_MAX(FRAME_HEADER_LEN + plen + 4);
}

static void enqueue_frame(link_t *lk, uint8_t type, uint8_t ch_id,
                           const uint8_t *payload, size_t plen) {
    if (!lk->up) return;
    if (plen > MAX_PAYLOAD) {
        LOG("oversized frame type=0x%02x ch=%u plen=%zu", type, ch_id, plen);
        link_close(lk);
        return;
    }

    static uint8_t dec[FRAME_DEC_MAX];
    static uint8_t enc[FRAME_ENC_MAX + 1];
    uint8_t header[FRAME_HEADER_LEN];

    header[0] = type;
    header[1] = ch_id;
    pik_put_u32le(header + 2, lk->tx_session);
    header[6] = (uint8_t)lk->tx_seq;
    header[7] = (uint8_t)(lk->tx_seq >> 8);

    size_t enc_len = 0;
    if (pik_frame_encode(header, sizeof(header), payload, plen,
                         dec, sizeof(dec), enc, sizeof(enc), &enc_len) != PIK_FRAME_OK) {
        LOG("cobs_encode failed type=0x%02x ch=%u", type, ch_id);
        link_close(lk);
        return;
    }

    if (!lk_push(lk, enc, enc_len)) {
        LOG("link TX ring full, closing before dropping frame type=0x%02x ch=%u",
            type, ch_id);
        link_close(lk);
        return;
    }
    lk->tx_seq++;

    if (!(lk->epev & EPOLLOUT) && lk->fd >= 0) {
        lk->epev |= EPOLLOUT;
        pik_epoll_set(g_epfd, lk->fd, lk->epev, lk);
    }
}

static bool link_parse_rx(link_t *lk) {
    pik_frame_status_t st = pik_frame_rx_consume(lk->rxbuf, &lk->rxbuf_len,
                                                 sizeof(lk->rxbuf),
                                                 dispatch_rx_frame, lk);
    if (st == PIK_FRAME_CALLBACK_FAILED)
        return false;
    if (st == PIK_FRAME_RX_OVERFLOW) {
        LOG("link RX overflow");
        link_close(lk);
        return false;
    }
    return true;
}

// ── serial port ───────────────────────────────────────────────────────────────
static int open_serial(const char *path, int baud) {
    int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) return -1;
    if (tty_set_byte_raw_baud(fd, baud) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

// ── MCU channel ───────────────────────────────────────────────────────────────
static void mcu_on_link_up(channel_t *c, link_t *lk) {
    if (c->mcu_state == MCU_ACTIVE)
        enqueue_frame(lk, F_READY, c->ch_id, NULL, 0);
    else
        enqueue_frame(lk, F_FLUSH, c->ch_id, NULL, 0);
}

static void mcu_open(channel_t *c, link_t *lk, int64_t now) {
    if (c->fd >= 0) return;
    c->fd = open_serial(c->dev, c->baud);
    if (c->fd < 0) return;
    c->last_byte_ms = now;
    c->epev = 0;
    chan_epoll_update(c);
    if (lk->up) mcu_on_link_up(c, lk);
}

static void ch_send_data(link_t *lk, channel_t *c, const uint8_t *buf, size_t n) {
    size_t off = 0;
    while (off < n) {
        size_t chunk = n - off;
        if (chunk > MAX_PAYLOAD) chunk = MAX_PAYLOAD;
        enqueue_frame(lk, F_DATA, c->ch_id, buf + off, chunk);
        off += chunk;
    }
}

static bool ch_link_full(channel_t *c, link_t *lk) {
    if (link_can_queue_frame(lk, MAX_PAYLOAD)) return false;
    lk->paused = true;
    chan_pause(c);
    return true;
}

static void mcu_on_readable(channel_t *c, link_t *lk, int64_t now) {
    if (ch_link_full(c, lk)) return;

    static uint8_t buf[MAX_PAYLOAD];
    ssize_t n = read(c->fd, buf, sizeof(buf));
    if (n <= 0) {
        if (n == 0 || (n < 0 && (errno == EAGAIN || errno == EINTR))) return;
        LOG("ch%u UART error: %s", c->ch_id, strerror(errno));
        pik_epoll_del(g_epfd, c->fd);
        close(c->fd);
        c->fd = -1;
        c->epev = 0;
        return;
    }
    c->last_byte_ms = now;

    switch (c->mcu_state) {
    case MCU_RESETTING:
    case MCU_INIT:
        for (ssize_t i = 0; i < n; i++) {
            if (buf[i] != 0x7E) continue;
            c->mcu_state = MCU_ACTIVE;
            LOG("ch%u MCU active", c->ch_id);
            enqueue_frame(lk, F_READY, c->ch_id, NULL, 0);
            ch_send_data(lk, c, buf + i, (size_t)(n - i));
            return;
        }
        break;
    case MCU_ACTIVE:
        ch_send_data(lk, c, buf, (size_t)n);
        break;
    }
}

static bool mcu_on_writable(channel_t *c) {
    if (!chan_drain(c)) {
        LOG("ch%u MCU write failure, closing link", c->ch_id);
        return false;
    }
    chan_epoll_update(c);
    return true;
}

static bool mcu_on_frame(channel_t *c, uint8_t type, const uint8_t *payload, size_t plen) {
    if (type != F_DATA) {
        LOG("ch%u unexpected MCU frame type=0x%02x", c->ch_id, type);
        return false;
    }
    if (c->fd < 0) {
        LOG("ch%u data for closed MCU", c->ch_id);
        return false;
    }
    if (chan_space(c) < plen) {
        LOG("ch%u MCU txbuf full, closing link before dropping %zu bytes", c->ch_id, plen);
        return false;
    }
    chan_push(c, payload, plen);
    chan_epoll_update(c);
    return true;
}

static void mcu_tick(channel_t *c, link_t *lk, int64_t now) {
    if (c->fd < 0) {
        mcu_open(c, lk, now);
        return;
    }
    if (c->mcu_state == MCU_ACTIVE && (now - c->last_byte_ms) > RESET_SILENCE_MS) {
        LOG("ch%u MCU silence timeout, resetting", c->ch_id);
        c->mcu_state = MCU_RESETTING;
        enqueue_frame(lk, F_FLUSH, c->ch_id, NULL, 0);
    }
}

static int64_t mcu_deadline(const channel_t *c, int64_t now) {
    if (c->fd < 0) return now + 1000;
    if (c->mcu_state == MCU_ACTIVE) return c->last_byte_ms + RESET_SILENCE_MS;
    return INT64_MAX;
}

// ── PTY channel ───────────────────────────────────────────────────────────────
static void pty_open(channel_t *c) {
    if (c->fd >= 0) return;
    char name[64];
    if (openpty(&c->fd, &c->slave_fd, name, NULL, NULL) < 0) {
        LOG("ch%u openpty: %s", c->ch_id, strerror(errno));
        return;
    }
    pik_fd_set_nonblock(c->fd);
    unlink(c->pty_path);
    if (symlink(name, c->pty_path) < 0)
        LOG("ch%u symlink %s: %s", c->ch_id, c->pty_path, strerror(errno));
    else
        LOG("ch%u PTY %s -> %s", c->ch_id, c->pty_path, name);
    c->epev = 0;
    chan_epoll_update(c);
}

static void pty_close(channel_t *c) {
    if (c->fd < 0) return;
    pik_epoll_del(g_epfd, c->fd);
    close(c->fd);
    c->fd = -1;
    c->epev = 0;
    if (c->slave_fd >= 0) { close(c->slave_fd); c->slave_fd = -1; }
    unlink(c->pty_path);
    c->tx_head = c->tx_tail = 0;
    LOG("ch%u PTY closed", c->ch_id);
}

static void pty_on_readable(channel_t *c, link_t *lk) {
    if (ch_link_full(c, lk)) return;

    static uint8_t buf[MAX_PAYLOAD];
    ssize_t n = read(c->fd, buf, sizeof(buf));
    if (n <= 0) {
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) return;
        pty_close(c);
        return;
    }
    ch_send_data(lk, c, buf, (size_t)n);
}

static bool pty_on_writable(channel_t *c) {
    if (!chan_drain(c)) {
        pty_close(c);
        return true;
    }
    chan_epoll_update(c);
    return true;
}

static bool pty_on_frame(channel_t *c, uint8_t type, const uint8_t *payload, size_t plen) {
    switch (type) {
    case F_DATA:
        if (c->fd < 0) {
            LOG("ch%u data for closed PTY", c->ch_id);
            return false;
        }
        if (chan_space(c) < plen) {
            LOG("ch%u PTY txbuf full, closing link before dropping %zu bytes", c->ch_id, plen);
            return false;
        }
        chan_push(c, payload, plen);
        chan_epoll_update(c);
        break;
    case F_FLUSH: pty_close(c); break;
    case F_READY: pty_open(c);  break;
    default:
        LOG("ch%u unexpected PTY frame type=0x%02x", c->ch_id, type);
        return false;
    }
    return true;
}

// ── channel dispatch ──────────────────────────────────────────────────────────
static channel_t *find_channel(uint8_t ch_id) {
    for (int i = 0; i < g_n_chans; i++)
        if (g_chans[i].ch_id == ch_id) return &g_chans[i];
    return NULL;
}

static void ch_on_readable(channel_t *c, link_t *lk, int64_t now) {
    if (c->type == CH_MCU) mcu_on_readable(c, lk, now);
    else                    pty_on_readable(c, lk);
}

static bool ch_on_writable(channel_t *c) {
    if (c->type == CH_MCU) return mcu_on_writable(c);
    return pty_on_writable(c);
}

static void ch_tick(channel_t *c, link_t *lk, int64_t now) {
    if (c->type == CH_MCU) mcu_tick(c, lk, now);
}

static int64_t ch_deadline(const channel_t *c, int64_t now) {
    if (c->type == CH_MCU) return mcu_deadline(c, now);
    return INT64_MAX;
}

// ── frame dispatch ────────────────────────────────────────────────────────────
static void dispatch_frame(link_t *lk, const uint8_t *enc, size_t enc_len) {
    static uint8_t dec[FRAME_DEC_MAX];
    pik_frame_t frame;

    pik_frame_status_t st = pik_frame_decode(enc, enc_len, FRAME_ENC_MAX + 1,
                                             FRAME_HEADER_LEN, dec, sizeof(dec), &frame);
    if (st != PIK_FRAME_OK) {
        link_fail_frame(lk, pik_frame_status_text(st), enc, enc_len);
        return;
    }

    uint8_t        type    = frame.header[0];
    uint8_t        ch_id   = frame.header[1];
    uint32_t       session = pik_get_u32le(frame.header + 2);
    uint16_t       seq     = (uint16_t)frame.header[6] | (uint16_t)frame.header[7] << 8;
    size_t         plen    = frame.payload_len;
    const uint8_t *payload = frame.payload;

    if (session == 0) {
        LOG("link failure: zero session type=0x%02x ch=%u", type, ch_id);
        link_close(lk);
        return;
    }
    if (lk->rx_session == 0) {
        if (seq != 0) {
            LOG("link failure: first frame seq=%u type=0x%02x ch=%u", seq, type, ch_id);
            link_close(lk);
            return;
        }
        lk->rx_session = session;
        lk->rx_seq = 1;
    } else {
        if (session != lk->rx_session) {
            LOG("link failure: session changed old=0x%08x new=0x%08x type=0x%02x ch=%u",
                lk->rx_session, session, type, ch_id);
            link_close(lk);
            return;
        }
        if (seq != lk->rx_seq) {
            LOG("link failure: seq gap session=0x%08x seq=%u expected=%u type=0x%02x ch=%u",
                session, seq, lk->rx_seq, type, ch_id);
            link_close(lk);
            return;
        }
        lk->rx_seq++;
    }

    channel_t *c = find_channel(ch_id);
    if (!c) {
        LOG("link failure: unknown channel id=%u type=0x%02x", ch_id, type);
        link_close(lk);
        return;
    }

    bool ok = (c->type == CH_MCU) ? mcu_on_frame(c, type, payload, plen)
                                  : pty_on_frame(c, type, payload, plen);
    if (!ok)
        link_close(lk);
}

static bool dispatch_rx_frame(void *ctx, const uint8_t *enc, size_t enc_len) {
    dispatch_frame((link_t *)ctx, enc, enc_len);
    return !g_session_failed;
}

// ── link management ───────────────────────────────────────────────────────────
static void link_close(link_t *lk) {
    g_session_failed = true;
    if (lk->fd >= 0) {
        pik_epoll_del(g_epfd, lk->fd);
        close(lk->fd);
        lk->fd = -1;
        lk->epev = 0;
    }
    if (lk->up) {
        lk->up = false;
        LOG("link down");
        for (int i = 0; i < g_n_chans; i++) {
            channel_t *c = &g_chans[i];
            if (c->type == CH_PTY) pty_close(c);
        }
    }
}

static bool link_open(link_t *lk, const char *dev) {
    lk->fd = open(dev, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (lk->fd < 0) {
        LOG("link open %s: %s", dev, strerror(errno));
        g_session_failed = true;
        return false;
    }

    if (tty_set_byte_raw(lk->fd) < 0) {
        LOG("termios setup failed on %s: %s", dev, strerror(errno));
        close(lk->fd);
        lk->fd = -1;
        g_session_failed = true;
        return false;
    }

    lk->rxbuf_len = 0;
    lk->tx_head = lk->tx_tail = 0;
    if (!lk->tx_session)
        lk->tx_session = pik_session_id(0);
    lk->rx_session = 0;
    lk->tx_seq = lk->rx_seq = 0;
    lk->up   = true;
    lk->epev = EPOLLIN;
    pik_epoll_set(g_epfd, lk->fd, EPOLLIN, lk);

    LOG("link opened: %s", dev);
    LOG("link up");
    for (int i = 0; i < g_n_chans; i++) {
        channel_t *c = &g_chans[i];
        if (c->type == CH_MCU) mcu_on_link_up(c, lk);
    }
    return true;
}

static void link_on_readable(link_t *lk) {
    size_t space = sizeof(lk->rxbuf) - lk->rxbuf_len;
    if (!space) {
        LOG("link RX overflow");
        link_close(lk);
        return;
    }

    ssize_t n = read(lk->fd, lk->rxbuf + lk->rxbuf_len, space);
    if (n <= 0) {
        if (n == 0 || (n < 0 && (errno == EAGAIN || errno == EINTR))) return;
        LOG("link read: %s", strerror(errno));
        link_close(lk);
        return;
    }
    lk->rxbuf_len += (size_t)n;
    if (!link_parse_rx(lk)) return;
}

static void link_on_writable(link_t *lk) {
    lk_drain(lk);
    if (lk->paused && lk_avail(lk) < LINK_LOW_WATER) {
        lk->paused = false;
        for (int i = 0; i < g_n_chans; i++)
            chan_resume(&g_chans[i]);
    }
}

static void link_tick(link_t *lk) {
    if (lk->fd < 0) return;
    if (!lk->paused && lk_avail(lk) > LINK_HIGH_WATER) {
        lk->paused = true;
        for (int i = 0; i < g_n_chans; i++)
            chan_pause(&g_chans[i]);
    }
}

// ── component API ─────────────────────────────────────────────────────────────
void serialmux_init(const serialmux_config_t *cfg, int epfd) {
    g_epfd = epfd;
    g_session_failed = false;
    memset(&g_link, 0, sizeof(g_link));
    g_link.fd = -1;

    g_n_chans = cfg->n_channels;
    for (int i = 0; i < g_n_chans; i++) {
        const ch_spec_t *s = &cfg->channels[i];
        channel_t *c = &g_chans[i];
        memset(c, 0, sizeof(*c));
        c->type     = s->type;
        c->ch_id    = s->ch_id;
        c->fd       = -1;
        c->slave_fd = -1;
        if (s->type == CH_MCU) {
            c->baud      = s->baud;
            c->mcu_state = MCU_INIT;
            snprintf(c->dev,      sizeof(c->dev),      "%s", s->dev);
        } else {
            snprintf(c->pty_path, sizeof(c->pty_path), "%s", s->path);
        }
    }
}

bool serialmux_start(const char *link_dev, int64_t now) {
    memset(&g_link, 0, sizeof(g_link));
    g_link.fd = -1;
    g_link.tx_session = pik_session_id(now);
    g_session_failed = false;
    for (int i = 0; i < g_n_chans; i++) {
        channel_t *c = &g_chans[i];
        c->paused = false;
        c->epev = 0;
        c->tx_head = c->tx_tail = 0;
        if (c->type == CH_MCU) {
            c->mcu_state = MCU_INIT;
            c->last_byte_ms = now;
        }
    }
    return link_open(&g_link, link_dev);
}

bool serialmux_dispatch(void *ptr, uint32_t events, int64_t now) {
    if (g_session_failed) return false;
    if (ptr == &g_link) {
        if (events & (EPOLLERR | EPOLLHUP)) {
            link_close(&g_link);
            return false;
        }
        if (events & EPOLLIN) link_on_readable(&g_link);
        if (!g_session_failed && (events & EPOLLOUT)) link_on_writable(&g_link);
        return !g_session_failed;
    }

    channel_t *c = (channel_t *)ptr;
    if (events & (EPOLLERR | EPOLLHUP)) {
        pik_epoll_del(g_epfd, c->fd);
        close(c->fd);
        c->fd = -1;
        c->epev = 0;
        return true;
    }
    if (events & EPOLLIN) ch_on_readable(c, &g_link, now);
    if (!g_session_failed && (events & EPOLLOUT) && !ch_on_writable(c)) {
        link_close(&g_link);
        return false;
    }
    return !g_session_failed;
}

bool serialmux_tick(int64_t now) {
    if (g_session_failed) return false;
    link_tick(&g_link);
    for (int i = 0; i < g_n_chans && !g_session_failed; i++)
        ch_tick(&g_chans[i], &g_link, now);
    return !g_session_failed;
}

int64_t serialmux_deadline(int64_t now) {
    int64_t dl = INT64_MAX;
    for (int i = 0; i < g_n_chans; i++) {
        int64_t cd = ch_deadline(&g_chans[i], now);
        if (cd < dl) dl = cd;
    }
    return dl;
}

void serialmux_cleanup(void) {
    if (g_link.fd >= 0) {
        pik_epoll_del(g_epfd, g_link.fd);
        close(g_link.fd);
        g_link.fd = -1;
        g_link.epev = 0;
    }
    g_link.up = false;
    g_link.paused = false;
    g_link.tx_head = g_link.tx_tail = 0;
    g_link.rxbuf_len = 0;

    for (int i = 0; i < g_n_chans; i++) {
        channel_t *c = &g_chans[i];
        if (c->fd >= 0) {
            pik_epoll_del(g_epfd, c->fd);
            close(c->fd);
            c->fd = -1;
        }
        if (c->slave_fd >= 0) {
            close(c->slave_fd);
            c->slave_fd = -1;
        }
        if (c->type == CH_PTY)
            unlink(c->pty_path);
        c->paused = false;
        c->epev = 0;
        c->tx_head = c->tx_tail = 0;
    }
    g_session_failed = false;
}
