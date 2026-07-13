// src/serialmux.c — COBS serial multiplexer, library interface

#include "serialmux.h"
#include "serialmux_proto.h"
#include "nanocobs/cobs.h"
#include "fd.h"
#include "frame.h"
#include "link.h"
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

// ── sizing ────────────────────────────────────────────────────────────────────
#define FRAME_DEC_MAX   (PIK_SERIALMUX_FRAME_HEADER_LEN + PIK_SERIALMUX_MAX_PAYLOAD + 4)
#define FRAME_ENC_MAX   COBS_ENCODE_MAX(FRAME_DEC_MAX)

#define LINK_RING_CAP   (1u << 20)
#define LINK_HIGH_WATER (LINK_RING_CAP / 2)
#define LINK_LOW_WATER  (LINK_RING_CAP / 4)

#define CHAN_RING_CAP   (1u << 16)
#define CHAN_RING_MASK  (CHAN_RING_CAP - 1u)

#define HIST_CAP        (1u << 18)
#define HIST_SLOTS      512u

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

// ── globals ───────────────────────────────────────────────────────────────────
static pik_link_t g_link;
static bool       g_link_paused;   /* high-water pause of all channels */
static channel_t  g_chans[MAX_CHANNELS];
static int        g_n_chans;
static int        g_epfd = -1;

static uint8_t             s_link_tx[LINK_RING_CAP];
static uint8_t             s_link_rx[FRAME_ENC_MAX + 4];
static uint8_t             s_link_hist[HIST_CAP];
static pik_link_hist_ent_t s_link_hist_ent[HIST_SLOTS];

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
static void mcu_on_link_up(channel_t *c) {
    if (c->mcu_state == MCU_ACTIVE)
        pik_link_enqueue(&g_link, PIK_SERIALMUX_FRAME_READY, c->ch_id, NULL, 0);
    else
        pik_link_enqueue(&g_link, PIK_SERIALMUX_FRAME_FLUSH, c->ch_id, NULL, 0);
}

static void mcu_open(channel_t *c, int64_t now) {
    if (c->fd >= 0) return;
    c->fd = open_serial(c->dev, c->baud);
    if (c->fd < 0) return;
    c->last_byte_ms = now;
    c->epev = 0;
    chan_epoll_update(c);
    if (g_link.fd >= 0) mcu_on_link_up(c);
}

static void ch_send_data(channel_t *c, const uint8_t *buf, size_t n) {
    size_t off = 0;
    while (off < n) {
        size_t chunk = n - off;
        if (chunk > PIK_SERIALMUX_MAX_PAYLOAD) chunk = PIK_SERIALMUX_MAX_PAYLOAD;
        pik_link_enqueue(&g_link, PIK_SERIALMUX_FRAME_DATA, c->ch_id, buf + off, chunk);
        off += chunk;
    }
}

static bool ch_link_full(channel_t *c) {
    if (pik_link_can_queue(&g_link, PIK_SERIALMUX_MAX_PAYLOAD)) return false;
    g_link_paused = true;
    chan_pause(c);
    return true;
}

static void mcu_on_readable(channel_t *c, int64_t now) {
    if (ch_link_full(c)) return;

    static uint8_t buf[PIK_SERIALMUX_MAX_PAYLOAD];
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
            pik_link_enqueue(&g_link, PIK_SERIALMUX_FRAME_READY, c->ch_id, NULL, 0);
            ch_send_data(c, buf + i, (size_t)(n - i));
            return;
        }
        break;
    case MCU_ACTIVE:
        ch_send_data(c, buf, (size_t)n);
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
    if (type != PIK_SERIALMUX_FRAME_DATA) {
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

static void mcu_tick(channel_t *c, int64_t now) {
    if (c->fd < 0) {
        mcu_open(c, now);
        return;
    }
    if (c->mcu_state == MCU_ACTIVE && (now - c->last_byte_ms) > RESET_SILENCE_MS) {
        LOG("ch%u MCU silence timeout, resetting", c->ch_id);
        c->mcu_state = MCU_RESETTING;
        pik_link_enqueue(&g_link, PIK_SERIALMUX_FRAME_FLUSH, c->ch_id, NULL, 0);
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

static void pty_on_readable(channel_t *c) {
    if (ch_link_full(c)) return;

    static uint8_t buf[PIK_SERIALMUX_MAX_PAYLOAD];
    ssize_t n = read(c->fd, buf, sizeof(buf));
    if (n <= 0) {
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) return;
        pty_close(c);
        return;
    }
    ch_send_data(c, buf, (size_t)n);
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
    case PIK_SERIALMUX_FRAME_DATA:
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
    case PIK_SERIALMUX_FRAME_FLUSH: pty_close(c); break;
    case PIK_SERIALMUX_FRAME_READY: pty_open(c);  break;
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

static void ch_on_readable(channel_t *c, int64_t now) {
    if (c->type == CH_MCU) mcu_on_readable(c, now);
    else                    pty_on_readable(c);
}

static bool ch_on_writable(channel_t *c) {
    if (c->type == CH_MCU) return mcu_on_writable(c);
    return pty_on_writable(c);
}

static void ch_tick(channel_t *c, int64_t now) {
    if (c->type == CH_MCU) mcu_tick(c, now);
}

static int64_t ch_deadline(const channel_t *c, int64_t now) {
    if (c->type == CH_MCU) return mcu_deadline(c, now);
    return INT64_MAX;
}

// ── link callbacks ────────────────────────────────────────────────────────────
static bool mux_on_frame(void *ctx, uint8_t type, uint8_t ch_id,
                         const uint8_t *payload, size_t plen) {
    (void)ctx;
    channel_t *c = find_channel(ch_id);
    if (!c) {
        LOG("link failure: unknown channel id=%u type=0x%02x", ch_id, type);
        return false;
    }
    return (c->type == CH_MCU) ? mcu_on_frame(c, type, payload, plen)
                               : pty_on_frame(c, type, payload, plen);
}

static void mux_on_down(void *ctx) {
    (void)ctx;
    LOG("link down");
    for (int i = 0; i < g_n_chans; i++) {
        channel_t *c = &g_chans[i];
        if (c->type == CH_PTY) pty_close(c);
    }
}

// ── component API ─────────────────────────────────────────────────────────────
void serialmux_init(const serialmux_config_t *cfg, int epfd) {
    g_epfd = epfd;
    g_link_paused = false;

    pik_link_cfg_t lcfg = {
        .name        = "mux",
        .nak_type    = PIK_SERIALMUX_FRAME_NAK,
        .has_aux     = true,
        .first_type  = 0,   /* first frame must carry seq 0 */
        .heal_from_zero = true, /* mux sessions restart in lockstep via the
                                 * control link, so a whole-session heal can
                                 * only reach a peer that consumed nothing */
        .max_payload = PIK_SERIALMUX_MAX_PAYLOAD,
        .txbuf       = s_link_tx,       .tx_cap     = LINK_RING_CAP,
        .rxbuf       = s_link_rx,       .rx_cap     = sizeof(s_link_rx),
        .hist        = s_link_hist,     .hist_cap   = HIST_CAP,
        .hist_ent    = s_link_hist_ent, .hist_slots = HIST_SLOTS,
        .on_frame    = mux_on_frame,
        .on_down     = mux_on_down,
        .ctx         = NULL,
    };
    pik_link_init(&g_link, &lcfg, epfd);

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
    g_link_paused = false;
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
    if (!pik_link_open(&g_link, link_dev, now))
        return false;
    LOG("link up");
    for (int i = 0; i < g_n_chans; i++) {
        channel_t *c = &g_chans[i];
        if (c->type == CH_MCU) mcu_on_link_up(c);
    }
    return true;
}

static void resume_channels_if_drained(void) {
    if (g_link_paused && pik_link_tx_avail(&g_link) < LINK_LOW_WATER) {
        g_link_paused = false;
        for (int i = 0; i < g_n_chans; i++)
            chan_resume(&g_chans[i]);
    }
}

bool serialmux_dispatch(void *ptr, uint32_t events, int64_t now) {
    if (g_link.failed) return false;
    if (pik_link_owns_event(&g_link, ptr)) {
        if (!pik_link_dispatch(&g_link, events, now))
            return false;
        resume_channels_if_drained();
        return true;
    }
    g_link.now_ms = now;

    channel_t *c = (channel_t *)ptr;
    if (events & (EPOLLERR | EPOLLHUP)) {
        pik_epoll_del(g_epfd, c->fd);
        close(c->fd);
        c->fd = -1;
        c->epev = 0;
        return true;
    }
    if (events & EPOLLIN) ch_on_readable(c, now);
    if (!g_link.failed && (events & EPOLLOUT) && !ch_on_writable(c)) {
        pik_link_fail(&g_link);
        return false;
    }
    return !g_link.failed;
}

bool serialmux_tick(int64_t now) {
    if (g_link.failed) return false;
    if (!g_link_paused && pik_link_tx_avail(&g_link) > LINK_HIGH_WATER) {
        g_link_paused = true;
        for (int i = 0; i < g_n_chans; i++)
            chan_pause(&g_chans[i]);
    }
    if (!pik_link_tick(&g_link, now))
        return false;
    resume_channels_if_drained();
    for (int i = 0; i < g_n_chans && !g_link.failed; i++)
        ch_tick(&g_chans[i], now);
    return !g_link.failed;
}

int64_t serialmux_deadline(int64_t now) {
    int64_t dl = INT64_MAX;
    for (int i = 0; i < g_n_chans; i++) {
        int64_t cd = ch_deadline(&g_chans[i], now);
        if (cd < dl) dl = cd;
    }
    int64_t ld = pik_link_deadline(&g_link);
    if (ld < dl) dl = ld;
    return dl;
}

void serialmux_cleanup(void) {
    pik_link_cleanup(&g_link);
    g_link_paused = false;

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
}
