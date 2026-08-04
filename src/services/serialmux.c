// Serial mux service: MCU/PTY data channels 1..8 on the shared link

#include "serialmux.h"
#include "pik_proto.h"
#include "session.h"
#include "fd.h"
#include "logging.h"
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
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#define MUX_HIGH_WATER  (PIK_SESSION_MUX_QUEUE_CAP / 2u)
#define MUX_LOW_WATER   (PIK_SESSION_MUX_QUEUE_CAP / 4u)

#define CHAN_RING_CAP   (1u << 16)
#define CHAN_RING_MASK  (CHAN_RING_CAP - 1u)

/*
 * Reset if an active MCU stops producing its regular heartbeat/status stream.
 * This should stay responsive: a healthy K1 MCU talks well inside this window.
 */
#define RESET_SILENCE_MS  5000

typedef enum { MCU_INIT, MCU_ACTIVE, MCU_RESETTING } mcu_state_t;

typedef struct channel {
    ch_type_t   type;
    uint8_t     ch_id;
    int         fd;
    bool        paused;
    uint32_t    epev;

    uint8_t     txbuf[CHAN_RING_CAP];
    uint32_t    tx_head, tx_tail;

    char        dev[128];
    int         baud;
    mcu_state_t mcu_state;
    int64_t     last_byte_ms;

    char        pty_path[128];
    int         slave_fd;
    bool        pty_peer_open;
    bool        pty_drop_logged;
} channel_t;

static bool       g_started;       /* session handshake completed */
static bool       g_link_paused;   /* high-water pause of all channels */
static channel_t  g_chans[MAX_CHANNELS];
static int        g_n_chans;
static int        g_epfd = -1;

#define LOG(...)  pik_log("mux", __VA_ARGS__)

static uint8_t wire_ch(const channel_t *c) {
    return pik_mux_cli_to_wire(c->ch_id);
}

static void pty_log_drop(channel_t *c, size_t n) {
    if (c->pty_drop_logged) return;
    LOG("ch%u PTY has no reader, dropping %zu bytes", c->ch_id, n);
    c->pty_drop_logged = true;
}

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
            if (w < 0 && c->type == CH_PTY && errno == EIO) {
                pty_log_drop(c, chan_avail(c));
                c->tx_head = c->tx_tail;
                c->pty_peer_open = false;
                break;
            }
            if (w < 0 && errno != EAGAIN && errno != EINTR) {
                LOG("ch%u write: %s", c->ch_id, strerror(errno));
                return false;
            }
            break;
        }
        if (c->type == CH_PTY)
            c->pty_drop_logged = false;
        c->tx_head += (uint32_t)w;
    }
    return true;
}

static bool chan_epoll_update(channel_t *c) {
    if (c->fd < 0) return true;
    uint32_t want = (!c->paused ? EPOLLIN : 0u) | (chan_avail(c) ? EPOLLOUT : 0u);
    if (c->type == CH_PTY && want)
        want |= EPOLLET;
    if (want == c->epev) return true;
    c->epev = want;
    if (want && !pik_epoll_set(g_epfd, c->fd, want, c)) {
        LOG("ch%u epoll update: %s", c->ch_id, strerror(errno));
        pik_session_fail();
        return false;
    }
    if (!want)
        pik_epoll_del(g_epfd, c->fd);
    return true;
}

static void chan_pause(channel_t *c) {
    if (!c->paused) {
        c->paused = true;
        (void)chan_epoll_update(c);
    }
}

static void chan_resume(channel_t *c) {
    if (c->paused) {
        c->paused = false;
        (void)chan_epoll_update(c);
    }
}

#ifndef CRTSCTS
#define CRTSCTS 0
#endif

static int baud_const(int baud) {
    switch (baud) {
    case 9600:   return B9600;
    case 19200:  return B19200;
    case 38400:  return B38400;
    case 57600:  return B57600;
    case 115200: return B115200;
    case 230400: return B230400;
    case 460800: return B460800;
    default:     return -1;
    }
}

static int tty_set_byte_raw(int fd, int baud) {
    struct termios t;
    if (tcgetattr(fd, &t) < 0)
        return -1;

    t.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | INPCK | ISTRIP |
                   IGNCR | ICRNL | IXON | IXOFF | IXANY);
    t.c_oflag &= ~OPOST;
    t.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    t.c_cflag &= ~(PARENB | PARODD | CSTOPB | CSIZE | CRTSCTS);
    t.c_cflag |= CS8 | CLOCAL | CREAD;
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;

    if (baud > 0) {
        int bc = baud_const(baud);
        if (bc < 0) {
            errno = EINVAL;
            return -1;
        }
        if (cfsetispeed(&t, (speed_t)bc) < 0 ||
            cfsetospeed(&t, (speed_t)bc) < 0)
            return -1;
    }
    return tcsetattr(fd, TCSANOW, &t);
}

static int open_serial(const char *path, int baud) {
    int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) return -1;
    if (tty_set_byte_raw(fd, baud) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void mcu_announce(channel_t *c) {
    if (c->mcu_state == MCU_ACTIVE)
        pik_session_enqueue(PIK_SESSION_CLASS_MUX, PIK_FRAME_MUX_READY,
                            wire_ch(c), NULL, 0);
    else
        pik_session_enqueue(PIK_SESSION_CLASS_MUX, PIK_FRAME_MUX_FLUSH,
                            wire_ch(c), NULL, 0);
}

static void mcu_reset(channel_t *c, bool send_flush) {
    if (c->fd >= 0) {
        pik_epoll_del(g_epfd, c->fd);
        close(c->fd);
    }
    c->fd = -1;
    c->epev = 0;
    c->mcu_state = MCU_INIT;
    c->tx_head = c->tx_tail = 0;
    if (send_flush)
        pik_session_enqueue(PIK_SESSION_CLASS_MUX, PIK_FRAME_MUX_FLUSH,
                            wire_ch(c), NULL, 0);
}

static void mcu_open(channel_t *c, int64_t now) {
    if (c->fd >= 0) return;
    c->fd = open_serial(c->dev, c->baud);
    if (c->fd < 0) return;
    c->last_byte_ms = now;
    c->epev = 0;
    if (!chan_epoll_update(c)) return;
    if (g_started) mcu_announce(c);
}

static void ch_send_data(channel_t *c, const uint8_t *buf, size_t n) {
    size_t off = 0;
    while (off < n) {
        size_t chunk = n - off;
        if (chunk > PIK_MUX_MAX_PAYLOAD) chunk = PIK_MUX_MAX_PAYLOAD;
        pik_session_enqueue(PIK_SESSION_CLASS_MUX, PIK_FRAME_MUX_DATA,
                            wire_ch(c), buf + off, chunk);
        off += chunk;
    }
}

static bool ch_link_full(channel_t *c) {
    if (pik_session_can_queue(PIK_SESSION_CLASS_MUX, PIK_MUX_MAX_PAYLOAD) &&
        pik_session_backlog(PIK_SESSION_CLASS_MUX) <= MUX_HIGH_WATER)
        return false;
    g_link_paused = true;
    chan_pause(c);
    return true;
}

static void mcu_on_readable(channel_t *c, int64_t now) {
    if (ch_link_full(c)) return;

    static uint8_t buf[PIK_MUX_MAX_PAYLOAD];
    ssize_t n = read(c->fd, buf, sizeof(buf));
    if (n <= 0) {
        if (n == 0 || (n < 0 && (errno == EAGAIN || errno == EINTR))) return;
        LOG("ch%u UART error: %s", c->ch_id, strerror(errno));
        mcu_reset(c, true);
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
            pik_session_enqueue(PIK_SESSION_CLASS_MUX, PIK_FRAME_MUX_READY,
                                wire_ch(c), NULL, 0);
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
    return chan_epoll_update(c);
}

static bool mcu_on_frame(channel_t *c, uint8_t type, const uint8_t *payload, size_t plen) {
    if (type != PIK_FRAME_MUX_DATA) {
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
    return chan_epoll_update(c);
}

static void mcu_tick(channel_t *c, int64_t now) {
    if (c->fd < 0) {
        mcu_open(c, now);
        return;
    }
    if (c->mcu_state == MCU_ACTIVE && (now - c->last_byte_ms) > RESET_SILENCE_MS) {
        LOG("ch%u MCU silence timeout, resetting", c->ch_id);
        c->mcu_state = MCU_RESETTING;
        pik_session_enqueue(PIK_SESSION_CLASS_MUX, PIK_FRAME_MUX_FLUSH,
                            wire_ch(c), NULL, 0);
    }
}

static int64_t mcu_deadline(const channel_t *c, int64_t now) {
    if (c->fd < 0) return now + 1000;
    if (c->mcu_state == MCU_ACTIVE) return c->last_byte_ms + RESET_SILENCE_MS;
    return INT64_MAX;
}

static void pty_open(channel_t *c) {
    if (c->fd >= 0) return;
    char name[64];
    if (openpty(&c->fd, &c->slave_fd, name, NULL, NULL) < 0) {
        LOG("ch%u openpty: %s", c->ch_id, strerror(errno));
        return;
    }
    if (tty_set_byte_raw(c->fd, 0) < 0 ||
        tty_set_byte_raw(c->slave_fd, 0) < 0) {
        LOG("ch%u PTY raw setup: %s", c->ch_id, strerror(errno));
        close(c->fd);
        close(c->slave_fd);
        c->fd = -1;
        c->slave_fd = -1;
        return;
    }
    if (chmod(name, 0666) < 0)
        LOG("ch%u chmod %s: %s", c->ch_id, name, strerror(errno));
    if (!pik_fd_set_nonblock(c->fd)) {
        LOG("ch%u PTY nonblocking setup: %s", c->ch_id, strerror(errno));
        close(c->fd);
        close(c->slave_fd);
        c->fd = -1;
        c->slave_fd = -1;
        return;
    }
    unlink(c->pty_path);
    if (symlink(name, c->pty_path) < 0)
        LOG("ch%u symlink %s: %s", c->ch_id, c->pty_path, strerror(errno));
    else
        LOG("ch%u PTY %s -> %s", c->ch_id, c->pty_path, name);
    close(c->slave_fd);
    c->slave_fd = -1;
    c->epev = 0;
    (void)chan_epoll_update(c);
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
    c->pty_peer_open = false;
    c->pty_drop_logged = false;
}

static bool pty_probe_peer(channel_t *c) {
    bool saw_peer = false;

    for (;;) {
        if (ch_link_full(c)) return c->pty_peer_open || saw_peer;

        static uint8_t buf[PIK_MUX_MAX_PAYLOAD];
        ssize_t n = read(c->fd, buf, sizeof(buf));
        if (n <= 0) {
            if (n < 0 && errno == EAGAIN) {
                c->pty_peer_open = true;
                return chan_epoll_update(c);
            }
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && errno == EIO) {
                c->pty_peer_open = false;
                (void)chan_epoll_update(c);
                return false;
            }
            pty_close(c);
            return false;
        }
        saw_peer = true;
        c->pty_peer_open = true;
        c->pty_drop_logged = false;
        ch_send_data(c, buf, (size_t)n);
        if (!chan_epoll_update(c)) return false;
    }
}

static void pty_on_readable(channel_t *c) {
    (void)pty_probe_peer(c);
}

static bool pty_on_writable(channel_t *c) {
    if (!chan_drain(c)) {
        pty_close(c);
        return true;
    }
    return chan_epoll_update(c);
}

static bool pty_on_frame(channel_t *c, uint8_t type, const uint8_t *payload, size_t plen) {
    switch (type) {
    case PIK_FRAME_MUX_DATA:
        if (c->fd < 0) {
            LOG("ch%u data for closed PTY", c->ch_id);
            return false;
        }
        if (!c->pty_peer_open && !pty_probe_peer(c)) {
            pty_log_drop(c, plen);
            return true;
        }
        if (chan_space(c) < plen) {
            LOG("ch%u PTY txbuf full, closing link before dropping %zu bytes", c->ch_id, plen);
            return false;
        }
        chan_push(c, payload, plen);
        if (!chan_epoll_update(c)) return false;
        break;
    case PIK_FRAME_MUX_FLUSH:
        if (plen != 0) {
            LOG("ch%u malformed FLUSH len=%zu", c->ch_id, plen);
            return false;
        }
        pty_close(c);
        break;
    case PIK_FRAME_MUX_READY:
        if (plen != 0) {
            LOG("ch%u malformed READY len=%zu", c->ch_id, plen);
            return false;
        }
        pty_open(c);
        break;
    default:
        LOG("ch%u unexpected PTY frame type=0x%02x", c->ch_id, type);
        return false;
    }
    return true;
}

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
    else if (c->fd >= 0 && !c->pty_peer_open) pty_probe_peer(c);
}

static int64_t ch_deadline(const channel_t *c, int64_t now) {
    if (c->type == CH_MCU) return mcu_deadline(c, now);
    if (c->fd >= 0 && !c->pty_peer_open) return now + 100;
    return INT64_MAX;
}

bool serialmux_on_frame(uint8_t type, uint8_t wire_id,
                        const uint8_t *payload, size_t plen) {
    channel_t *c = find_channel(pik_mux_wire_to_cli(wire_id));
    if (!c) {
        LOG("link failure: unknown channel id=%u type=0x%02x", wire_id, type);
        return false;
    }
    return (c->type == CH_MCU) ? mcu_on_frame(c, type, payload, plen)
                               : pty_on_frame(c, type, payload, plen);
}

void serialmux_on_link_down(void) {
    if (!g_started) return;
    LOG("link down");
    g_started = false;
    for (int i = 0; i < g_n_chans; i++) {
        channel_t *c = &g_chans[i];
        if (c->type == CH_PTY) pty_close(c);
    }
}

void serialmux_init(const serialmux_config_t *cfg, int epfd) {
    g_epfd = epfd;
    g_started = false;
    g_link_paused = false;

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

void serialmux_start(int64_t now) {
    g_started = true;
    g_link_paused = false;
    for (int i = 0; i < g_n_chans; i++) {
        channel_t *c = &g_chans[i];
        c->paused = false;
        c->tx_head = c->tx_tail = 0;
        if (c->type == CH_MCU) {
            if (c->fd >= 0) {
                c->epev = 0;
                (void)chan_epoll_update(c);
            }
            c->mcu_state = MCU_INIT;
            c->last_byte_ms = now;
            if (c->fd >= 0) mcu_announce(c);
        }
    }
}

static void resume_channels_if_drained(void) {
    if (g_link_paused &&
        pik_session_backlog(PIK_SESSION_CLASS_MUX) < MUX_LOW_WATER &&
        pik_session_can_queue(PIK_SESSION_CLASS_MUX, PIK_MUX_MAX_PAYLOAD)) {
        g_link_paused = false;
        for (int i = 0; i < g_n_chans; i++)
            chan_resume(&g_chans[i]);
    }
}

bool serialmux_owns_event(const void *ptr) {
    for (int i = 0; i < g_n_chans; i++)
        if (ptr == &g_chans[i]) return true;
    return false;
}

bool serialmux_dispatch(void *ptr, uint32_t events, int64_t now) {
    channel_t *c = (channel_t *)ptr;
    if (events & (EPOLLERR | EPOLLHUP)) {
        if (c->type == CH_PTY) {
            c->pty_peer_open = false;
            if (chan_avail(c)) {
                pty_log_drop(c, chan_avail(c));
                c->tx_head = c->tx_tail;
            }
            return chan_epoll_update(c) && pik_session_up();
        }
    }
    if (events & (EPOLLERR | EPOLLHUP)) {
        mcu_reset(c, true);
        return true;
    }
    if (c->type == CH_PTY && events)
        c->pty_peer_open = true;
    if (events & EPOLLIN) ch_on_readable(c, now);
    if (!pik_session_up()) return false;
    if ((events & EPOLLOUT) && !ch_on_writable(c)) {
        pik_session_fail();
        return false;
    }
    return pik_session_up();
}

bool serialmux_tick(int64_t now) {
    if (!g_started) return true;
    if (!pik_session_up()) return false;
    if (!g_link_paused &&
        pik_session_backlog(PIK_SESSION_CLASS_MUX) > MUX_HIGH_WATER) {
        g_link_paused = true;
        for (int i = 0; i < g_n_chans; i++)
            chan_pause(&g_chans[i]);
    }
    resume_channels_if_drained();
    for (int i = 0; i < g_n_chans && pik_session_up(); i++)
        ch_tick(&g_chans[i], now);
    return pik_session_up();
}

int64_t serialmux_deadline(int64_t now) {
    if (!g_started) return INT64_MAX;
    int64_t dl = INT64_MAX;
    for (int i = 0; i < g_n_chans; i++) {
        int64_t cd = ch_deadline(&g_chans[i], now);
        if (cd < dl) dl = cd;
    }
    return dl;
}

void serialmux_cleanup(void) {
    g_started = false;
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
