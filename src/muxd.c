// src/muxd.c — multi-MCU serial multiplexer, COBS framing, no TCP

#include "cobs.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pty.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

// ── frame types ───────────────────────────────────────────────────────────────
#define F_DATA   0x01u
#define F_FLUSH  0x02u
#define F_READY  0x03u
#define F_HELLO  0x05u
#define F_PING   0x20u
#define F_PONG   0x21u

// ── sizing ────────────────────────────────────────────────────────────────────
#define MAX_CHANNELS    8
#define MAX_PAYLOAD     4096

// decoded frame: [type:1][ch:1][payload:0..4096][crc32:4]
#define FRAME_DEC_MAX   (2 + MAX_PAYLOAD + 4)
#define FRAME_ENC_MAX   COBS_ENCODE_MAX(FRAME_DEC_MAX)

// link TX ring: power-of-2
#define LINK_RING_CAP   (1u << 20)
#define LINK_RING_MASK  (LINK_RING_CAP - 1u)
#define LINK_HIGH_WATER (LINK_RING_CAP / 2)
#define LINK_LOW_WATER  (LINK_RING_CAP / 4)

// channel TX ring: power-of-2
#define CHAN_RING_CAP   (1u << 16)
#define CHAN_RING_MASK  (CHAN_RING_CAP - 1u)

// timing (ms)
#define RESET_SILENCE_MS  5000
#define PING_IDLE_TX_MS   3000
#define LINK_DEAD_RX_MS   10000
#define RECONNECT_MIN_MS  500
#define RECONNECT_MAX_MS  8000

// ── types ─────────────────────────────────────────────────────────────────────
typedef enum { CH_MCU, CH_PTY } ch_type_t;
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

    int64_t     last_tx_ms;
    int64_t     last_rx_ms;
    int64_t     reconnect_deadline;
    int         reconnect_backoff_ms;

    char        vidpid[16];
} link_t;

// ── globals ───────────────────────────────────────────────────────────────────
static link_t    g_link;
static channel_t g_chans[MAX_CHANNELS];
static int       g_n_chans;
static int       g_epfd = -1;

// ── logging ───────────────────────────────────────────────────────────────────
static void log_msg(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}
#define LOG(...)  log_msg(__VA_ARGS__)
#define DIE(...)  do { log_msg(__VA_ARGS__); exit(1); } while(0)

// ── time ──────────────────────────────────────────────────────────────────────
static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// ── CRC32 (poly 0xEDB88320) ───────────────────────────────────────────────────
static uint32_t crc32_table[256];

static void crc32_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++)
            c = (c >> 1) ^ (0xEDB88320u & -(c & 1u));
        crc32_table[i] = c;
    }
}

static uint32_t crc32_calc(const uint8_t *buf, size_t len) {
    uint32_t c = 0xFFFFFFFFu;
    while (len--)
        c = (c >> 8) ^ crc32_table[(c ^ *buf++) & 0xFF];
    return c ^ 0xFFFFFFFFu;
}

// ── epoll helpers ─────────────────────────────────────────────────────────────
static void ep_set(int fd, uint32_t events, void *ptr) {
    struct epoll_event ev = { .events = events, .data.ptr = ptr };
    if (epoll_ctl(g_epfd, EPOLL_CTL_MOD, fd, &ev) == -1)
        epoll_ctl(g_epfd, EPOLL_CTL_ADD, fd, &ev);
}

static void ep_del(int fd) {
    epoll_ctl(g_epfd, EPOLL_CTL_DEL, fd, NULL);
}

// ── channel ring helpers ──────────────────────────────────────────────────────
static uint32_t chan_avail(const channel_t *c) { return c->tx_tail - c->tx_head; }
static uint32_t chan_space(const channel_t *c) { return CHAN_RING_CAP - chan_avail(c); }

static void chan_push(channel_t *c, const uint8_t *src, size_t len) {
    for (size_t i = 0; i < len; i++)
        c->txbuf[c->tx_tail++ & CHAN_RING_MASK] = src[i];
}

static void chan_drain(channel_t *c) {
    while (chan_avail(c)) {
        uint32_t off    = c->tx_head & CHAN_RING_MASK;
        uint32_t contig = CHAN_RING_CAP - off;
        uint32_t avail  = chan_avail(c);
        size_t   n      = avail < contig ? avail : contig;
        ssize_t  w      = write(c->fd, c->txbuf + off, n);
        if (w <= 0) break;
        c->tx_head += (uint32_t)w;
    }
}

static void chan_epoll_update(channel_t *c) {
    if (c->fd < 0) return;
    uint32_t want = (!c->paused ? EPOLLIN : 0u) | (chan_avail(c) ? EPOLLOUT : 0u);
    if (want == c->epev) return;
    c->epev = want;
    if (want)
        ep_set(c->fd, want, c);
    else
        ep_del(c->fd);
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

// Drain link TX ring to fd; returns true if any bytes were written.
static bool lk_drain(link_t *lk) {
    bool wrote = false;
    while (lk_avail(lk)) {
        uint32_t off    = lk->tx_head & LINK_RING_MASK;
        uint32_t contig = LINK_RING_CAP - off;
        uint32_t avail  = lk_avail(lk);
        size_t   n      = avail < contig ? avail : contig;
        ssize_t  w      = write(lk->fd, lk->txbuf + off, n);
        if (w <= 0) break;
        lk->tx_head += (uint32_t)w;
        wrote = true;
    }
    uint32_t want = EPOLLIN | (lk_avail(lk) ? EPOLLOUT : 0u);
    if (want != lk->epev && lk->fd >= 0) {
        lk->epev = want;
        ep_set(lk->fd, want, lk);
    }
    return wrote;
}

// ── frame I/O ────────────────────────────────────────────────────────────────
static void dispatch_frame(link_t *lk, const uint8_t *enc, size_t enc_len);

static void enqueue_frame(link_t *lk, uint8_t type, uint8_t ch_id,
                           const uint8_t *payload, size_t plen) {
    if (!lk->up && type != F_HELLO) return;
    if (plen > MAX_PAYLOAD) return;

    static uint8_t dec[FRAME_DEC_MAX];
    static uint8_t enc[FRAME_ENC_MAX + 1];

    dec[0] = type;
    dec[1] = ch_id;
    if (plen) memcpy(dec + 2, payload, plen);

    uint32_t crc = crc32_calc(dec, 2 + plen);
    dec[2 + plen + 0] = (uint8_t)(crc);
    dec[2 + plen + 1] = (uint8_t)(crc >> 8);
    dec[2 + plen + 2] = (uint8_t)(crc >> 16);
    dec[2 + plen + 3] = (uint8_t)(crc >> 24);

    // cobs_encode appends the 0x00 delimiter and includes it in enc_len
    size_t enc_len = 0;
    if (cobs_encode(dec, 2 + plen + 4, enc, FRAME_ENC_MAX + 1, &enc_len) != COBS_RET_SUCCESS)
        return;

    if (!lk_push(lk, enc, enc_len)) {
        LOG("link TX ring full, dropping frame type=0x%02x ch=%u", type, ch_id);
        return;
    }

    if (!(lk->epev & EPOLLOUT) && lk->fd >= 0) {
        lk->epev |= EPOLLOUT;
        ep_set(lk->fd, lk->epev, lk);
    }
}

static void link_parse_rx(link_t *lk) {
    while (true) {
        uint8_t *delim = memchr(lk->rxbuf, COBS_FRAME_DELIMITER, lk->rxbuf_len);
        if (!delim) break;
        size_t frame_len = (size_t)(delim - lk->rxbuf);
        if (frame_len > 0)
            dispatch_frame(lk, lk->rxbuf, frame_len + 1); // include 0x00: cobs_decode requires it
        size_t consumed = frame_len + 1;
        lk->rxbuf_len -= consumed;
        memmove(lk->rxbuf, lk->rxbuf + consumed, lk->rxbuf_len);
    }
    if (lk->rxbuf_len >= sizeof(lk->rxbuf)) {
        LOG("link RX overflow, discarding");
        lk->rxbuf_len = 0;
    }
}

// ── serial port ───────────────────────────────────────────────────────────────
static void set_nonblock(int fd) {
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
}

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

static int open_serial(const char *path, int baud) {
    int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) return -1;
    int bc = baud_const(baud);
    if (bc < 0) { close(fd); errno = EINVAL; return -1; }
    struct termios t;
    tcgetattr(fd, &t);
    cfmakeraw(&t);
    cfsetispeed(&t, (speed_t)bc);
    cfsetospeed(&t, (speed_t)bc);
    t.c_cflag |= CLOCAL | CREAD;
    tcsetattr(fd, TCSANOW, &t);
    return fd;
}

// ── USB sysfs discovery ───────────────────────────────────────────────────────
#define SYSPATH_MAX 512

static bool sysfs_read(const char *path, char *buf, size_t cap) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    ssize_t n = read(fd, buf, cap - 1);
    close(fd);
    if (n <= 0) return false;
    buf[n] = '\0';
    char *nl = strchr(buf, '\n');
    if (nl) *nl = '\0';
    return true;
}

static bool find_usb_dir(const char *sysfs_dev_path, char *out_dir, size_t out_cap) {
    char cur[SYSPATH_MAX];
    if (!realpath(sysfs_dev_path, cur)) return false;
    for (int i = 0; i < 8; i++) {
        char test[SYSPATH_MAX + 16];
        snprintf(test, sizeof(test), "%s/idVendor", cur);
        if (access(test, R_OK) == 0) {
            snprintf(out_dir, out_cap, "%s", cur);
            return true;
        }
        char *slash = strrchr(cur, '/');
        if (!slash || slash == cur) break;
        *slash = '\0';
    }
    return false;
}

static void to_lower(char *s) {
    for (; *s; s++)
        if (*s >= 'A' && *s <= 'F') *s += 32;
}

// Strip leading "0x"/"0X" and lowercase — configfs stores VID/PID as "0x1d6b".
static const char *strip_0x(char *s) {
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    to_lower(s);
    return s;
}

// Check configfs for a USB gadget with matching vid:pid (Pi gadget side).
// gadget devices don't expose idVendor via the normal USB host sysfs hierarchy.
static bool configfs_gadget_matches(const char *want_vid, const char *want_pid) {
    DIR *d = opendir("/sys/kernel/config/usb_gadget");
    if (!d) return false;
    struct dirent *ent;
    bool found = false;
    while (!found && (ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        char path[SYSPATH_MAX + 32], vid[16], pid[16];
        snprintf(path, sizeof(path),
                 "/sys/kernel/config/usb_gadget/%s/idVendor", ent->d_name);
        if (!sysfs_read(path, vid, sizeof(vid))) continue;
        snprintf(path, sizeof(path),
                 "/sys/kernel/config/usb_gadget/%s/idProduct", ent->d_name);
        if (!sysfs_read(path, pid, sizeof(pid))) continue;
        found = strcmp(strip_0x(vid), want_vid) == 0 &&
                strcmp(strip_0x(pid), want_pid) == 0;
    }
    closedir(d);
    return found;
}

// Returns static buffer with /dev/ttyXXXN path, or NULL.
static const char *find_usb_tty(const char *vidpid) {
    char want_vid[8], want_pid[8];
    if (sscanf(vidpid, "%7[^:]:%7s", want_vid, want_pid) != 2) return NULL;
    to_lower(want_vid);
    to_lower(want_pid);

    static const char *prefixes[] = { "ttyACM", "ttyGS", NULL };
    DIR *d = opendir("/sys/class/tty");
    if (!d) return NULL;

    static char result[64];
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        bool match = false;
        for (int i = 0; prefixes[i]; i++)
            if (strncmp(ent->d_name, prefixes[i], strlen(prefixes[i])) == 0) { match = true; break; }
        if (!match) continue;

        char devlink[SYSPATH_MAX];
        snprintf(devlink, sizeof(devlink), "/sys/class/tty/%s/device", ent->d_name);

        // ttyGS (gadget) devices: fall back to configfs vid:pid check
        if (strncmp(ent->d_name, "ttyGS", 5) == 0) {
            if (configfs_gadget_matches(want_vid, want_pid)) {
                snprintf(result, sizeof(result), "/dev/%.50s", ent->d_name);
                closedir(d);
                return result;
            }
            continue;
        }

        char usb_dir[SYSPATH_MAX];
        if (!find_usb_dir(devlink, usb_dir, sizeof(usb_dir))) continue;

        char vid[8], pid[8], tmp[SYSPATH_MAX + 16];
        snprintf(tmp, sizeof(tmp), "%s/idVendor", usb_dir);
        if (!sysfs_read(tmp, vid, sizeof(vid))) continue;
        snprintf(tmp, sizeof(tmp), "%s/idProduct", usb_dir);
        if (!sysfs_read(tmp, pid, sizeof(pid))) continue;
        to_lower(vid);
        to_lower(pid);

        if (strcmp(vid, want_vid) == 0 && strcmp(pid, want_pid) == 0) {
            snprintf(result, sizeof(result), "/dev/%.50s", ent->d_name);
            closedir(d);
            return result;
        }
    }
    closedir(d);
    return NULL;
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

static void mcu_send_data(link_t *lk, channel_t *c, const uint8_t *buf, size_t n) {
    size_t off = 0;
    while (off < n) {
        size_t chunk = n - off;
        if (chunk > MAX_PAYLOAD) chunk = MAX_PAYLOAD;
        enqueue_frame(lk, F_DATA, c->ch_id, buf + off, chunk);
        off += chunk;
    }
}

static void mcu_on_readable(channel_t *c, link_t *lk, int64_t now) {
    static uint8_t buf[MAX_PAYLOAD];
    ssize_t n = read(c->fd, buf, sizeof(buf));
    if (n <= 0) {
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) return;
        LOG("ch%u UART error: %s", c->ch_id, strerror(errno));
        ep_del(c->fd);
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
            mcu_send_data(lk, c, buf + i, (size_t)(n - i));
            return;
        }
        break;
    case MCU_ACTIVE:
        mcu_send_data(lk, c, buf, (size_t)n);
        break;
    }
}

static void mcu_on_writable(channel_t *c) {
    chan_drain(c);
    chan_epoll_update(c);
}

static void mcu_on_frame(channel_t *c, uint8_t type, const uint8_t *payload, size_t plen) {
    if (type != F_DATA || c->fd < 0) return;
    if (chan_space(c) < plen) {
        LOG("ch%u MCU txbuf full, dropping %zu bytes", c->ch_id, plen);
        return;
    }
    chan_push(c, payload, plen);
    chan_epoll_update(c);
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
    set_nonblock(c->fd);
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
    ep_del(c->fd);
    close(c->fd);
    c->fd = -1;
    c->epev = 0;
    if (c->slave_fd >= 0) { close(c->slave_fd); c->slave_fd = -1; }
    unlink(c->pty_path);
    c->tx_head = c->tx_tail = 0;
    LOG("ch%u PTY closed", c->ch_id);
}

static void pty_on_readable(channel_t *c, link_t *lk) {
    static uint8_t buf[MAX_PAYLOAD];
    ssize_t n = read(c->fd, buf, sizeof(buf));
    if (n <= 0) {
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) return;
        pty_close(c);
        return;
    }
    size_t off = 0;
    while (off < (size_t)n) {
        size_t chunk = (size_t)n - off;
        if (chunk > MAX_PAYLOAD) chunk = MAX_PAYLOAD;
        enqueue_frame(lk, F_DATA, c->ch_id, buf + off, chunk);
        off += chunk;
    }
}

static void pty_on_writable(channel_t *c) {
    chan_drain(c);
    chan_epoll_update(c);
}

static void pty_on_frame(channel_t *c, uint8_t type, const uint8_t *payload, size_t plen) {
    switch (type) {
    case F_DATA:
        if (c->fd < 0) return;
        if (chan_space(c) < plen) {
            LOG("ch%u PTY txbuf full, dropping %zu bytes", c->ch_id, plen);
            return;
        }
        chan_push(c, payload, plen);
        chan_epoll_update(c);
        break;
    case F_FLUSH: pty_close(c); break;
    case F_READY: pty_open(c);  break;
    default: break;
    }
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

static void ch_on_writable(channel_t *c) {
    if (c->type == CH_MCU) mcu_on_writable(c);
    else                    pty_on_writable(c);
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
    size_t dec_len = 0;

    if (cobs_decode(enc, enc_len, dec, sizeof(dec), &dec_len) != COBS_RET_SUCCESS) {
        LOG("COBS decode failed enc_len=%zu", enc_len);
        return;
    }
    if (dec_len < 6) return; // type(1) + ch(1) + crc32(4)

    uint8_t        type    = dec[0];
    uint8_t        ch_id   = dec[1];
    size_t         plen    = dec_len - 6;
    const uint8_t *payload = dec + 2;

    uint32_t got = (uint32_t)dec[dec_len - 4]
                 | (uint32_t)dec[dec_len - 3] << 8
                 | (uint32_t)dec[dec_len - 2] << 16
                 | (uint32_t)dec[dec_len - 1] << 24;
    if (crc32_calc(dec, 2 + plen) != got) {
        LOG("CRC mismatch type=0x%02x ch=%u dec_len=%zu", type, ch_id, dec_len);
        return;
    }

    switch (type) {
    case F_HELLO:
        if (!lk->up) {
            lk->up = true;
            LOG("link up");
            enqueue_frame(lk, F_HELLO, 0, NULL, 0);
            for (int i = 0; i < g_n_chans; i++) {
                channel_t *c = &g_chans[i];
                if (c->type == CH_MCU) mcu_on_link_up(c, lk);
                // PTY host waits for F_READY from exporter
            }
        }
        return;
    case F_PING:
        enqueue_frame(lk, F_PONG, 0, NULL, 0);
        return;
    case F_PONG:
        return;
    default: break;
    }

    channel_t *c = find_channel(ch_id);
    if (!c) return;

    if (c->type == CH_MCU) mcu_on_frame(c, type, payload, plen);
    else                    pty_on_frame(c, type, payload, plen);
}

// ── link management ───────────────────────────────────────────────────────────
static void link_close(link_t *lk, int64_t now) {
    if (lk->fd >= 0) {
        ep_del(lk->fd);
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
            // MCU channels stay open on link drop
        }
    }
    lk->reconnect_deadline = now + lk->reconnect_backoff_ms;
    lk->reconnect_backoff_ms *= 2;
    if (lk->reconnect_backoff_ms > RECONNECT_MAX_MS)
        lk->reconnect_backoff_ms = RECONNECT_MAX_MS;
}

static void link_try_open(link_t *lk, int64_t now) {
    const char *dev = find_usb_tty(lk->vidpid);
    if (!dev) return;

    lk->fd = open(dev, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (lk->fd < 0) return;

    struct termios t;
    tcgetattr(lk->fd, &t);
    cfmakeraw(&t);
    cfsetispeed(&t, B115200);
    cfsetospeed(&t, B115200);
    t.c_cflag |= CLOCAL | CREAD;
    tcsetattr(lk->fd, TCSANOW, &t);

    lk->rxbuf_len = 0;
    lk->tx_head = lk->tx_tail = 0;
    lk->last_rx_ms = lk->last_tx_ms = now;
    lk->reconnect_backoff_ms = RECONNECT_MIN_MS;
    lk->up   = false;
    lk->epev = EPOLLIN;
    ep_set(lk->fd, EPOLLIN, lk);

    LOG("link opened: %s", dev);
    enqueue_frame(lk, F_HELLO, 0, NULL, 0);
}

static void link_on_readable(link_t *lk, int64_t now) {
    size_t space = sizeof(lk->rxbuf) - lk->rxbuf_len;
    if (!space) { lk->rxbuf_len = 0; space = sizeof(lk->rxbuf); }

    ssize_t n = read(lk->fd, lk->rxbuf + lk->rxbuf_len, space);
    if (n <= 0) {
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) return;
        LOG("link read: %s", n == 0 ? "EOF" : strerror(errno));
        link_close(lk, now);
        return;
    }
    lk->last_rx_ms = now;
    lk->rxbuf_len += (size_t)n;
    link_parse_rx(lk);
}

static void link_on_writable(link_t *lk, int64_t now) {
    if (lk_drain(lk))
        lk->last_tx_ms = now;
    if (lk->paused && lk_avail(lk) < LINK_LOW_WATER) {
        lk->paused = false;
        for (int i = 0; i < g_n_chans; i++)
            chan_resume(&g_chans[i]);
    }
}

static void link_tick(link_t *lk, int64_t now) {
    if (lk->fd < 0) {
        if (now >= lk->reconnect_deadline)
            link_try_open(lk, now);
        return;
    }
    // Retransmit HELLO until peer responds; covers race between reconnects.
    if (!lk->up && (now - lk->last_tx_ms) > 2000)
        enqueue_frame(lk, F_HELLO, 0, NULL, 0);
    if (lk->up) {
        if ((now - lk->last_tx_ms) > PING_IDLE_TX_MS)
            enqueue_frame(lk, F_PING, 0, NULL, 0);
        if ((now - lk->last_rx_ms) > LINK_DEAD_RX_MS) {
            LOG("link RX timeout");
            link_close(lk, now);
            return;
        }
    }
    if (!lk->paused && lk_avail(lk) > LINK_HIGH_WATER) {
        lk->paused = true;
        for (int i = 0; i < g_n_chans; i++)
            chan_pause(&g_chans[i]);
    }
}

static int64_t link_deadline(const link_t *lk) {
    if (lk->fd < 0) return lk->reconnect_deadline;
    if (!lk->up) return lk->last_tx_ms + 2000;
    int64_t a = lk->last_tx_ms + PING_IDLE_TX_MS;
    int64_t b = lk->last_rx_ms + LINK_DEAD_RX_MS;
    return a < b ? a : b;
}

// ── main event loop ───────────────────────────────────────────────────────────
#define MAX_EPOLL_EVENTS 16

static void run(void) {
    g_epfd = epoll_create1(EPOLL_CLOEXEC);
    if (g_epfd < 0) DIE("epoll_create1: %s", strerror(errno));

    int64_t now = now_ms();
    link_try_open(&g_link, now);

    for (;;) {
        now = now_ms();

        int64_t dl = link_deadline(&g_link);
        for (int i = 0; i < g_n_chans; i++) {
            int64_t cd = ch_deadline(&g_chans[i], now);
            if (cd < dl) dl = cd;
        }

        int timeout = (dl <= now) ? 0 : (int)(dl - now);
        if (timeout > 5000) timeout = 5000;

        struct epoll_event evs[MAX_EPOLL_EVENTS];
        int n = epoll_wait(g_epfd, evs, MAX_EPOLL_EVENTS, timeout);
        if (n < 0) {
            if (errno == EINTR) continue;
            DIE("epoll_wait: %s", strerror(errno));
        }

        now = now_ms();

        for (int i = 0; i < n; i++) {
            void    *ptr = evs[i].data.ptr;
            uint32_t ev  = evs[i].events;

            if (ptr == &g_link) {
                if (ev & (EPOLLERR | EPOLLHUP)) { link_close(&g_link, now); continue; }
                if (ev & EPOLLIN)  link_on_readable(&g_link, now);
                if (ev & EPOLLOUT) link_on_writable(&g_link, now);
            } else {
                channel_t *c = (channel_t *)ptr;
                if (ev & (EPOLLERR | EPOLLHUP)) {
                    ep_del(c->fd);
                    close(c->fd);
                    c->fd = -1;
                    c->epev = 0;
                    continue;
                }
                if (ev & EPOLLIN)  ch_on_readable(c, &g_link, now);
                if (ev & EPOLLOUT) ch_on_writable(c);
            }
        }

        now = now_ms();
        link_tick(&g_link, now);
        for (int i = 0; i < g_n_chans; i++)
            ch_tick(&g_chans[i], &g_link, now);
    }
}

// ── argument parsing ──────────────────────────────────────────────────────────
static void usage(const char *prog) {
    fprintf(stderr,
        "Usage:\n"
        "  %s --usb VID:PID  mcu:N:DEV:BAUD [mcu:N:DEV:BAUD ...]\n"
        "  %s --usb VID:PID  pty:N:SYMLINK  [pty:N:SYMLINK  ...]\n",
        prog, prog);
    exit(1);
}

int main(int argc, char **argv) {
    crc32_init();

    if (argc < 4) usage(argv[0]);

    int argi = 1;
    if (strcmp(argv[argi], "--usb") != 0 || argi + 1 >= argc) usage(argv[0]);
    strncpy(g_link.vidpid, argv[argi + 1], sizeof(g_link.vidpid) - 1);
    argi += 2;

    if (argi >= argc) usage(argv[0]);

    bool is_mcu = strncmp(argv[argi], "mcu:", 4) == 0;
    bool is_pty = strncmp(argv[argi], "pty:", 4) == 0;
    if (!is_mcu && !is_pty) usage(argv[0]);

    g_n_chans = 0;
    g_link.fd = -1;
    g_link.reconnect_backoff_ms = RECONNECT_MIN_MS;

    while (argi < argc && g_n_chans < MAX_CHANNELS) {
        char *spec = argv[argi++];
        channel_t *c = &g_chans[g_n_chans];
        memset(c, 0, sizeof(*c));
        c->fd = -1;
        c->slave_fd = -1;

        if (is_mcu) {
            char baud_str[16];
            if (sscanf(spec, "mcu:%hhu:%127[^:]:%15s", &c->ch_id, c->dev, baud_str) != 3)
                DIE("bad mcu spec: %s", spec);
            c->baud = atoi(baud_str);
            c->type = CH_MCU;
            c->mcu_state = MCU_INIT;
        } else {
            if (sscanf(spec, "pty:%hhu:%127s", &c->ch_id, c->pty_path) != 2)
                DIE("bad pty spec: %s", spec);
            c->type = CH_PTY;
        }
        g_n_chans++;
    }

    if (!g_n_chans) usage(argv[0]);

    LOG("mode=%s channels=%d", is_mcu ? "exporter" : "host", g_n_chans);
    run();
    return 0;
}
