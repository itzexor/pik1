#include "control.h"
#include "nanocobs/cobs.h"
#include "fd.h"
#include "frame.h"
#include "logging.h"
#include "tty.h"
#include "util.h"
#include "version.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>

#define C_HELLO   0x01u
#define C_PING    0x02u
#define C_PONG    0x03u
#define C_COMMAND 0x04u
#define C_ACK     0x05u

#define HELLO_MAGIC_0 'P'
#define HELLO_MAGIC_1 'I'
#define HELLO_MAGIC_2 'K'
#define HELLO_MAGIC_3 '1'
#define HELLO_LEN 29u
#define RELEASE_LEN 16u

#define MAX_PAYLOAD   128u
#define FRAME_DEC_MAX (1u + MAX_PAYLOAD + 4u)
#define FRAME_ENC_MAX COBS_ENCODE_MAX(FRAME_DEC_MAX)

#define TX_RING_CAP   8192u
#define TX_RING_MASK  (TX_RING_CAP - 1u)

#define HELLO_RETRY_MS 2000
#define PING_IDLE_MS  3000
#define LINK_DEAD_MS  10000

typedef struct {
    int fd;
    bool ready;
    bool failed;
    uint32_t epev;

    uint8_t txbuf[TX_RING_CAP];
    uint32_t tx_head, tx_tail;

    uint8_t rxbuf[FRAME_ENC_MAX + 4];
    size_t rxbuf_len;

    int64_t last_tx_ms;
    int64_t last_rx_ms;

    pik_control_role_t role;
    pik_control_command_fn on_command;
    void *ctx;

    uint32_t next_request_id;
    bool ack_pending;
    uint32_t ack_request_id;
    uint8_t ack_status;
} control_t;

static control_t g_ctrl;
static int g_epfd = -1;
static int g_ctrl_tag;

static void log_msg(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fputs("[ctrl] ", stderr);
    vfprintf(stderr, fmt, ap); va_end(ap);
    fputc('\n', stderr);
}
#define LOG(...) log_msg(__VA_ARGS__)

static uint32_t avail(void) { return g_ctrl.tx_tail - g_ctrl.tx_head; }
static uint32_t space(void) { return TX_RING_CAP - avail(); }

const char *pik_control_action_name(pik_control_action_t action) {
    switch (action) {
    case PIK_CONTROL_ACTION_RESTART_EXPORTER: return "restart-exporter";
    case PIK_CONTROL_ACTION_REBOOT_EXPORTER: return "reboot-exporter";
    case PIK_CONTROL_ACTION_POWEROFF_EXPORTER: return "poweroff-exporter";
    default: return "unknown";
    }
}

static bool action_valid(pik_control_action_t action) {
    switch (action) {
    case PIK_CONTROL_ACTION_RESTART_EXPORTER:
    case PIK_CONTROL_ACTION_REBOOT_EXPORTER:
    case PIK_CONTROL_ACTION_POWEROFF_EXPORTER:
        return true;
    default:
        return false;
    }
}

static bool tx_push(const uint8_t *src, size_t len) {
    if (space() < len) return false;
    for (size_t i = 0; i < len; i++)
        g_ctrl.txbuf[g_ctrl.tx_tail++ & TX_RING_MASK] = src[i];
    return true;
}

static void clear_pending_ack(void) {
    g_ctrl.ack_pending = false;
    g_ctrl.ack_request_id = 0;
    g_ctrl.ack_status = 0;
}

static void update_epoll(void) {
    if (g_ctrl.fd < 0) return;
    uint32_t want = EPOLLIN | (avail() ? EPOLLOUT : 0u);
    if (want == g_ctrl.epev) return;
    g_ctrl.epev = want;
    pik_epoll_set(g_epfd, g_ctrl.fd, want, &g_ctrl_tag);
}

static bool enqueue_frame(uint8_t type, const uint8_t *payload, size_t plen) {
    if (plen > MAX_PAYLOAD) return false;

    static uint8_t dec[FRAME_DEC_MAX];
    static uint8_t enc[FRAME_ENC_MAX + 1];
    uint8_t header[1] = { type };
    size_t enc_len = 0;

    if (pik_frame_encode(header, sizeof(header), payload, plen,
                         dec, sizeof(dec), enc, sizeof(enc), &enc_len) != PIK_FRAME_OK) {
        LOG("encode failed type=0x%02x", type);
        return false;
    }
    if (!tx_push(enc, enc_len)) {
        LOG("TX ring full, drop type=0x%02x", type);
        return false;
    }
    update_epoll();
    return true;
}

static bool send_hello(void) {
    uint8_t p[HELLO_LEN];
    memset(p, 0, sizeof(p));
    p[0] = HELLO_MAGIC_0;
    p[1] = HELLO_MAGIC_1;
    p[2] = HELLO_MAGIC_2;
    p[3] = HELLO_MAGIC_3;
    pik_put_u32le(p + 4, PIK1_PROTOCOL_VERSION);
    pik_put_u32le(p + 8, PIK1_FEATURE_FLAGS);
    p[12] = (uint8_t)g_ctrl.role;
    snprintf((char *)p + 13, RELEASE_LEN, "%s", PIK1_RELEASE_VERSION);
    return enqueue_frame(C_HELLO, p, sizeof(p));
}

static void close_link(void) {
    g_ctrl.failed = true;
    if (g_ctrl.fd >= 0) {
        pik_epoll_del(g_epfd, g_ctrl.fd);
        close(g_ctrl.fd);
        g_ctrl.fd = -1;
    }
    if (g_ctrl.ready) LOG("link down");
    g_ctrl.ready = false;
    g_ctrl.epev = 0;
    clear_pending_ack();
}

static bool handle_hello(const uint8_t *p, size_t len) {
    if (len != HELLO_LEN ||
        p[0] != HELLO_MAGIC_0 || p[1] != HELLO_MAGIC_1 ||
        p[2] != HELLO_MAGIC_2 || p[3] != HELLO_MAGIC_3) {
        LOG("bad HELLO");
        close_link();
        return false;
    }

    uint32_t proto = pik_get_u32le(p + 4);
    uint32_t features = pik_get_u32le(p + 8);
    uint8_t role = p[12];
    char release[RELEASE_LEN + 1];
    memcpy(release, p + 13, RELEASE_LEN);
    release[RELEASE_LEN] = '\0';

    if (proto != PIK1_PROTOCOL_VERSION) {
        LOG("protocol mismatch: local=%u remote=%u remote_release=%s",
            PIK1_PROTOCOL_VERSION, proto, release);
        close_link();
        return false;
    }
    if (role == (uint8_t)g_ctrl.role) {
        LOG("peer role mismatch: both sides role=%u", role);
        close_link();
        return false;
    }
    if (strcmp(release, PIK1_RELEASE_VERSION) != 0)
        LOG("release mismatch: local=%s remote=%s", PIK1_RELEASE_VERSION, release);

    if (!g_ctrl.ready) {
        g_ctrl.ready = true;
        LOG("link up: release=%s protocol=%u features=0x%08x",
            release, proto, features);
    }
    return true;
}

static bool dispatch_frame(const uint8_t *enc, size_t enc_len) {
    static uint8_t dec[FRAME_DEC_MAX];
    pik_frame_t frame;

    pik_frame_status_t st = pik_frame_decode(enc, enc_len, FRAME_ENC_MAX + 1,
                                             1, dec, sizeof(dec), &frame);
    if (st != PIK_FRAME_OK) {
        LOG("frame failure: %s", pik_frame_status_text(st));
        close_link();
        return false;
    }

    uint8_t type = frame.header[0];
    const uint8_t *p = frame.payload;
    size_t len = frame.payload_len;

    switch (type) {
    case C_HELLO:
        return handle_hello(p, len);
    case C_PING:
        enqueue_frame(C_PONG, NULL, 0);
        return true;
    case C_PONG:
        return true;
    case C_COMMAND:
        if (len != 5) return true;
        {
            pik_control_action_t action = (pik_control_action_t)p[4];
            uint32_t request_id = pik_get_u32le(p);
            if (!action_valid(action)) {
                LOG("rejecting unknown command action=%u request=%u", p[4], request_id);
                pik_control_send_ack(request_id, 1);
                return true;
            }
            if (g_ctrl.on_command)
                g_ctrl.on_command(action, request_id, g_ctrl.ctx);
        }
        return true;
    case C_ACK:
        if (len != 5) return true;
        g_ctrl.ack_request_id = pik_get_u32le(p);
        g_ctrl.ack_status = p[4];
        g_ctrl.ack_pending = true;
        return true;
    default:
        return true;
    }
}

static bool dispatch_rx_frame(void *ctx, const uint8_t *enc, size_t enc_len) {
    (void)ctx;
    return dispatch_frame(enc, enc_len);
}

static bool parse_rx(void) {
    pik_frame_status_t st = pik_frame_rx_consume(g_ctrl.rxbuf, &g_ctrl.rxbuf_len,
                                                 sizeof(g_ctrl.rxbuf),
                                                 dispatch_rx_frame, NULL);
    if (st == PIK_FRAME_CALLBACK_FAILED) return false;
    if (st == PIK_FRAME_RX_OVERFLOW) {
        LOG("RX overflow");
        close_link();
        return false;
    }
    return true;
}

static bool read_available(int64_t now) {
    while (g_ctrl.fd >= 0) {
        size_t cap = sizeof(g_ctrl.rxbuf) - g_ctrl.rxbuf_len;
        if (!cap) {
            if (!parse_rx()) return false;
            cap = sizeof(g_ctrl.rxbuf) - g_ctrl.rxbuf_len;
            if (!cap) {
                close_link();
                return false;
            }
        }
        ssize_t n = read(g_ctrl.fd, g_ctrl.rxbuf + g_ctrl.rxbuf_len, cap);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN) return true;
            LOG("read: %s", strerror(errno));
            close_link();
            return false;
        }
        if (n == 0) {
            LOG("read: EOF");
            close_link();
            return false;
        }
        g_ctrl.last_rx_ms = now;
        g_ctrl.rxbuf_len += (size_t)n;
        if (!parse_rx()) return false;
    }
    return false;
}

static bool drain_tx(int64_t now) {
    while (avail()) {
        uint32_t off = g_ctrl.tx_head & TX_RING_MASK;
        uint32_t contig = TX_RING_CAP - off;
        uint32_t n = avail();
        if (n > contig) n = contig;
        ssize_t w = write(g_ctrl.fd, g_ctrl.txbuf + off, n);
        if (w <= 0) {
            if (w < 0 && (errno == EAGAIN || errno == EINTR)) break;
            LOG("write: %s", w == 0 ? "EOF" : strerror(errno));
            close_link();
            return false;
        }
        g_ctrl.tx_head += (uint32_t)w;
        g_ctrl.last_tx_ms = now;
    }
    update_epoll();
    return true;
}

void pik_control_init(int epfd, pik_control_role_t role,
                      pik_control_command_fn on_command, void *ctx) {
    memset(&g_ctrl, 0, sizeof(g_ctrl));
    g_ctrl.fd = -1;
    g_ctrl.role = role;
    g_ctrl.on_command = on_command;
    g_ctrl.ctx = ctx;
    g_ctrl.next_request_id = 1;
    g_epfd = epfd;
}

bool pik_control_start(const char *dev, int64_t now) {
    int fd = open(dev, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        LOG("open %s: %s", dev, strerror(errno));
        g_ctrl.failed = true;
        return false;
    }
    if (tty_set_byte_raw(fd) < 0) {
        LOG("termios setup failed on %s: %s", dev, strerror(errno));
        close(fd);
        g_ctrl.failed = true;
        return false;
    }

    g_ctrl.fd = fd;
    g_ctrl.ready = false;
    g_ctrl.failed = false;
    g_ctrl.epev = EPOLLIN;
    g_ctrl.tx_head = g_ctrl.tx_tail = 0;
    g_ctrl.rxbuf_len = 0;
    g_ctrl.last_rx_ms = g_ctrl.last_tx_ms = now;
    pik_epoll_set(g_epfd, fd, EPOLLIN, &g_ctrl_tag);
    LOG("link opened: %s", dev);
    send_hello();
    return true;
}

bool pik_control_owns_event(void *ptr) {
    return ptr == &g_ctrl_tag;
}

bool pik_control_dispatch(void *ptr, uint32_t events, int64_t now) {
    if (!pik_control_owns_event(ptr)) return true;
    if (events & (EPOLLERR | EPOLLHUP)) {
        close_link();
        return false;
    }
    if (events & EPOLLIN)
        read_available(now);
    if (!g_ctrl.failed && (events & EPOLLOUT))
        drain_tx(now);
    return !g_ctrl.failed;
}

bool pik_control_tick(int64_t now) {
    if (g_ctrl.fd < 0 || g_ctrl.failed) return false;
    if (avail())
        drain_tx(now);
    if (!g_ctrl.ready && !avail() && (now - g_ctrl.last_tx_ms) > HELLO_RETRY_MS)
        send_hello();
    if (g_ctrl.ready) {
        if (!avail() && (now - g_ctrl.last_tx_ms) > PING_IDLE_MS)
            enqueue_frame(C_PING, NULL, 0);
        if ((now - g_ctrl.last_rx_ms) > LINK_DEAD_MS) {
            LOG("RX timeout");
            close_link();
            return false;
        }
    }
    return !g_ctrl.failed;
}

bool pik_control_ready(void) {
    return g_ctrl.ready;
}

int64_t pik_control_deadline(void) {
    if (g_ctrl.fd < 0) return INT64_MAX;
    if (!g_ctrl.ready) return g_ctrl.last_tx_ms + HELLO_RETRY_MS;
    int64_t a = g_ctrl.last_tx_ms + PING_IDLE_MS;
    int64_t b = g_ctrl.last_rx_ms + LINK_DEAD_MS;
    return a < b ? a : b;
}

void pik_control_cleanup(void) {
    if (g_ctrl.fd >= 0) {
        pik_epoll_del(g_epfd, g_ctrl.fd);
        close(g_ctrl.fd);
    }
    g_ctrl.fd = -1;
    g_ctrl.ready = false;
    g_ctrl.failed = false;
    g_ctrl.epev = 0;
    g_ctrl.tx_head = g_ctrl.tx_tail = 0;
    g_ctrl.rxbuf_len = 0;
    clear_pending_ack();
}

bool pik_control_send_command(pik_control_action_t action, uint32_t *request_id) {
    if (!g_ctrl.ready) return false;
    uint8_t p[5];
    uint32_t id = g_ctrl.next_request_id++;
    if (g_ctrl.next_request_id == 0) g_ctrl.next_request_id = 1;
    pik_put_u32le(p, id);
    p[4] = (uint8_t)action;
    if (!enqueue_frame(C_COMMAND, p, sizeof(p))) return false;
    if (request_id) *request_id = id;
    LOG("sent command %s request=%u", pik_control_action_name(action), id);
    return true;
}

bool pik_control_take_ack(uint32_t *request_id, uint8_t *status) {
    if (!g_ctrl.ack_pending) return false;
    if (request_id) *request_id = g_ctrl.ack_request_id;
    if (status) *status = g_ctrl.ack_status;
    g_ctrl.ack_pending = false;
    return true;
}

void pik_control_send_ack(uint32_t request_id, uint8_t status) {
    uint8_t p[5];
    pik_put_u32le(p, request_id);
    p[4] = status;
    enqueue_frame(C_ACK, p, sizeof(p));
}
