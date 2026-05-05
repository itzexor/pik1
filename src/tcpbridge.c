// src/tcpbridge.c — multi-connection TCP-over-serial bridge with ARQ
// Wire protocol: COBS + CRC32, frame layout: [type:1][conn_id:1][seq:1][payload][crc32:4]
// Runs on ttyGS1 (K1C) / ttyACM1 (Pi).

#include "nanocobs/cobs.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

// ── frame types ───────────────────────────────────────────────────────────────
#define TB_HELLO  0x10u
#define TB_PING   0x11u
#define TB_PONG   0x12u
#define TB_OPEN   0x20u
#define TB_DATA   0x21u
#define TB_CLOSE  0x22u
#define TB_PAUSE  0x23u
#define TB_RESUME 0x24u
#define TB_ACK    0x30u  // ARQ ack:  seq field = DATA seq successfully received
#define TB_NAK    0x31u  // ARQ nak:  seq field = next DATA seq expected (retransmit from here)

// ── sizing ────────────────────────────────────────────────────────────────────
#define MAX_CONNS        16
#define MAX_PAYLOAD      4088
#define FRAME_DEC_MAX    (3 + MAX_PAYLOAD + 4)   // type + conn_id + seq + payload + crc32
#define FRAME_ENC_MAX    COBS_ENCODE_MAX(FRAME_DEC_MAX)

#define CONN_RING_CAP    (1u << 18)               // 256 KB per connection
#define CONN_RING_MASK   (CONN_RING_CAP - 1u)
#define CONN_HIGH_WATER  (64u  * 1024u)
#define CONN_LOW_WATER   (16u  * 1024u)
#define LINK_RING_CAP    (1u << 17)               // 128 KB serial TX ring
#define LINK_RING_MASK   (LINK_RING_CAP - 1u)
#define LINK_HIGH_WATER  (32u  * 1024u)
#define LINK_LOW_WATER   (16u  * 1024u)
#define LINK_RX_CAP      (1u << 14)               // 16 KB serial RX buffer

#define ARQ_WINDOW       8                        // max unacked DATA frames in flight
#define ARQ_TIMEOUT_MS   200                      // retransmit oldest unacked after this

#define PING_IDLE_MS    3000
#define LINK_DEAD_MS    10000
#define RECONNECT_MIN   500
#define RECONNECT_MAX   8000

// ── types ─────────────────────────────────────────────────────────────────────
typedef struct {
    int      fd;
    uint32_t epev;
    uint8_t  txbuf[CONN_RING_CAP];
    uint32_t tx_head, tx_tail;
    bool     paused;       // link TX ring high-water
    bool     flow_paused;  // remote sent TB_PAUSE
    bool     pause_sent;   // we sent TB_PAUSE
    bool     arq_paused;   // ARQ window full
} conn_t;

typedef struct {
    int      fd;
    bool     up;
    bool     paused;
    uint32_t epev;
    uint8_t  txbuf[LINK_RING_CAP];
    uint32_t tx_head, tx_tail;
    uint8_t  rxbuf[LINK_RX_CAP];
    size_t   rxbuf_len;
    int64_t  last_tx_ms;
    int64_t  last_rx_ms;
    int64_t  reconnect_at;
    int      backoff_ms;
    char     dev[128];
} link_t;

typedef struct {
    uint8_t  enc[FRAME_ENC_MAX + 2];  // encoded frame including delimiter
    size_t   enc_len;
    bool     used;
    int64_t  sent_at_ms;
} arq_slot_t;

// ── globals ───────────────────────────────────────────────────────────────────
static conn_t     g_conns[MAX_CONNS];
static link_t     g_link;
static int        g_listen_fd  = -1;
static int        g_epfd       = -1;
static bool       g_is_listener;
static char       g_fwd_host[64];
static int        g_fwd_port;

// ARQ TX state (sender)
static arq_slot_t g_arq[ARQ_WINDOW]; // indexed by seq % ARQ_WINDOW
static uint8_t    g_tx_seq  = 0;     // next seq to assign
static uint8_t    g_tx_base = 0;     // oldest unACKed seq
// ARQ RX state (receiver)
static uint8_t    g_rx_next = 0;     // next expected incoming DATA seq
// window-full pause
static bool       g_arq_paused = false;

static int g_link_tag;
static int g_listen_tag;

// ── logging ───────────────────────────────────────────────────────────────────
static void log_msg(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fputs("[tcp] ", stderr);
    vfprintf(stderr, fmt, ap); va_end(ap);
    fputc('\n', stderr);
}
#define LOG(...) log_msg(__VA_ARGS__)
#define DIE(...) do { log_msg(__VA_ARGS__); exit(1); } while(0)

// ── time ──────────────────────────────────────────────────────────────────────
static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static bool parse_port(const char *s, int *out) {
    char *end = NULL;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno || !end || *end || v <= 0 || v > 65535) return false;
    *out = (int)v;
    return true;
}

// ── CRC32 ─────────────────────────────────────────────────────────────────────
static uint32_t crc32_table[256];
static void crc32_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++) c = (c >> 1) ^ (0xEDB88320u & -(c & 1u));
        crc32_table[i] = c;
    }
}
static uint32_t crc32(const uint8_t *p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    while (n--) c = (c >> 8) ^ crc32_table[(c ^ *p++) & 0xFF];
    return c ^ 0xFFFFFFFFu;
}

// ── epoll helpers ─────────────────────────────────────────────────────────────
static void ep_set(int fd, uint32_t ev, void *tag) {
    struct epoll_event e = { .events = ev, .data.ptr = tag };
    if (epoll_ctl(g_epfd, EPOLL_CTL_MOD, fd, &e) == -1)
        epoll_ctl(g_epfd, EPOLL_CTL_ADD, fd, &e);
}
static void ep_del(int fd) { epoll_ctl(g_epfd, EPOLL_CTL_DEL, fd, NULL); }

// ── connection ring helpers ───────────────────────────────────────────────────
static uint32_t conn_avail(const conn_t *c) { return c->tx_tail - c->tx_head; }
static uint32_t conn_space(const conn_t *c) { return CONN_RING_CAP - conn_avail(c); }

static void conn_push(conn_t *c, const uint8_t *src, size_t len) {
    for (size_t i = 0; i < len; i++)
        c->txbuf[c->tx_tail++ & CONN_RING_MASK] = src[i];
}

static void conn_drain(conn_t *c) {
    while (conn_avail(c) && c->fd >= 0) {
        uint32_t off    = c->tx_head & CONN_RING_MASK;
        uint32_t contig = CONN_RING_CAP - off;
        uint32_t avail  = conn_avail(c);
        size_t   n      = avail < contig ? avail : contig;
        ssize_t  w      = write(c->fd, c->txbuf + off, n);
        if (w <= 0) {
            if (w < 0 && errno != EAGAIN && errno != EINTR)
                LOG("conn %d TCP write: %s", (int)(c - g_conns), strerror(errno));
            break;
        }
        c->tx_head += (uint32_t)w;
    }
}

static void conn_epoll_update(conn_t *c) {
    if (c->fd < 0) return;
    uint32_t want = ((!c->paused && !c->flow_paused && !c->arq_paused) ? EPOLLIN : 0u)
                  | (conn_avail(c) ? EPOLLOUT : 0u);
    if (want == c->epev) return;
    c->epev = want;
    if (want) ep_set(c->fd, want, c);
    else      ep_del(c->fd);
}

// ── link ring helpers ─────────────────────────────────────────────────────────
static uint32_t lk_avail(void) { return g_link.tx_tail - g_link.tx_head; }
static uint32_t lk_space(void) { return LINK_RING_CAP - lk_avail(); }

static bool lk_push(const uint8_t *src, size_t len) {
    if (lk_space() < (uint32_t)len) return false;
    for (size_t i = 0; i < len; i++)
        g_link.txbuf[g_link.tx_tail++ & LINK_RING_MASK] = src[i];
    return true;
}

static void lk_drain(int64_t now) {
    link_t *lk = &g_link;
    while (lk_avail()) {
        uint32_t off    = lk->tx_head & LINK_RING_MASK;
        uint32_t contig = LINK_RING_CAP - off;
        uint32_t avail  = lk_avail();
        size_t   n      = avail < contig ? avail : contig;
        ssize_t  w      = write(lk->fd, lk->txbuf + off, n);
        if (w <= 0) {
            if (w < 0 && errno != EAGAIN && errno != EINTR)
                LOG("link write: %s", strerror(errno));
            break;
        }
        lk->tx_head += (uint32_t)w;
        lk->last_tx_ms = now;
    }
    uint32_t want = EPOLLIN | (lk_avail() ? EPOLLOUT : 0u);
    if (want != lk->epev) {
        lk->epev = want;
        ep_set(lk->fd, want, &g_link_tag);
    }
}

// ── flow control ──────────────────────────────────────────────────────────────
static void pause_all_conns(void) {
    if (g_link.paused) return;
    g_link.paused = true;
    for (int i = 0; i < MAX_CONNS; i++) {
        if (g_conns[i].fd < 0 || g_conns[i].paused) continue;
        g_conns[i].paused = true;
        conn_epoll_update(&g_conns[i]);
    }
}

static void resume_all_conns(void) {
    if (!g_link.paused) return;
    g_link.paused = false;
    for (int i = 0; i < MAX_CONNS; i++) {
        if (g_conns[i].fd < 0 || !g_conns[i].paused) continue;
        g_conns[i].paused = false;
        conn_epoll_update(&g_conns[i]);
    }
}

// ── ARQ window pause/resume ───────────────────────────────────────────────────
static void arq_pause_conns(void) {
    if (g_arq_paused) return;
    g_arq_paused = true;
    for (int i = 0; i < MAX_CONNS; i++) {
        if (g_conns[i].fd < 0) continue;
        g_conns[i].arq_paused = true;
        conn_epoll_update(&g_conns[i]);
    }
}

static void arq_resume_conns(void) {
    if (!g_arq_paused) return;
    g_arq_paused = false;
    for (int i = 0; i < MAX_CONNS; i++) {
        if (g_conns[i].fd < 0) continue;
        g_conns[i].arq_paused = false;
        conn_epoll_update(&g_conns[i]);
    }
}

// ── ARQ retransmit / ack / nak ────────────────────────────────────────────────
static void lk_arm_epollout(void) {
    if (!(g_link.epev & EPOLLOUT) && g_link.fd >= 0) {
        g_link.epev |= EPOLLOUT;
        ep_set(g_link.fd, g_link.epev, &g_link_tag);
    }
}

static void arq_retransmit_from(uint8_t seq, int64_t now) {
    uint8_t end = g_tx_seq;
    for (uint8_t s = seq; s != end; s++) {
        arq_slot_t *slot = &g_arq[s % ARQ_WINDOW];
        if (!slot->used) continue;
        if (!lk_push(slot->enc, slot->enc_len)) {
            LOG("arq: TX ring full during retransmit seq=%u", s);
            break;
        }
        slot->sent_at_ms = now;
    }
    lk_arm_epollout();
}

static void arq_ack(uint8_t seq) {
    uint8_t window = (uint8_t)(g_tx_seq - g_tx_base);
    if (window == 0) return;
    // seq must be within [g_tx_base, g_tx_seq)
    if ((uint8_t)(seq - g_tx_base) >= window) return;
    uint8_t new_base = (uint8_t)(seq + 1);
    while (g_tx_base != new_base) {
        g_arq[g_tx_base % ARQ_WINDOW].used = false;
        g_tx_base++;
    }
    if ((uint8_t)(g_tx_seq - g_tx_base) < ARQ_WINDOW)
        arq_resume_conns();
}

static void arq_nak(uint8_t seq, int64_t now) {
    uint8_t window = (uint8_t)(g_tx_seq - g_tx_base);
    if (window == 0) return;
    if ((uint8_t)(seq - g_tx_base) >= window) return;
    LOG("arq: NAK seq=%u retransmitting %u frames", seq, (uint8_t)(g_tx_seq - seq));
    arq_retransmit_from(seq, now);
}

static void arq_tick(int64_t now) {
    if ((uint8_t)(g_tx_seq - g_tx_base) == 0) return;
    arq_slot_t *oldest = &g_arq[g_tx_base % ARQ_WINDOW];
    if (oldest->used && (now - oldest->sent_at_ms) > ARQ_TIMEOUT_MS) {
        LOG("arq: timeout seq=%u window=%u retransmitting",
            g_tx_base, (uint8_t)(g_tx_seq - g_tx_base));
        arq_retransmit_from(g_tx_base, now);
    }
}

// ── frame encode ──────────────────────────────────────────────────────────────
// Encodes [type][conn_id][seq][payload][crc32] into out_enc; returns enc_len or 0.
static size_t frame_encode(uint8_t type, uint8_t conn_id, uint8_t seq,
                            const uint8_t *payload, size_t plen,
                            uint8_t *out_enc, size_t out_cap) {
    if (plen > MAX_PAYLOAD) return 0;
    static uint8_t dec[FRAME_DEC_MAX];
    dec[0] = type;
    dec[1] = conn_id;
    dec[2] = seq;
    if (plen) memcpy(dec + 3, payload, plen);
    uint32_t c = crc32(dec, 3 + plen);
    dec[3 + plen + 0] = (uint8_t)(c);
    dec[3 + plen + 1] = (uint8_t)(c >>  8);
    dec[3 + plen + 2] = (uint8_t)(c >> 16);
    dec[3 + plen + 3] = (uint8_t)(c >> 24);
    size_t enc_len = 0;
    if (cobs_encode(dec, 3 + plen + 4, out_enc, out_cap, &enc_len) != COBS_RET_SUCCESS)
        return 0;
    return enc_len;
}

// ── enqueue_ctrl: control frames (HELLO, PING, PONG, OPEN, CLOSE, PAUSE, RESUME, ACK, NAK)
// seq is meaningful for ACK/NAK; pass 0 for all others.
static void enqueue_ctrl(uint8_t type, uint8_t conn_id, uint8_t seq,
                          const uint8_t *payload, size_t plen) {
    static uint8_t enc[FRAME_ENC_MAX + 2];
    size_t enc_len = frame_encode(type, conn_id, seq, payload, plen, enc, sizeof(enc));
    if (!enc_len) { LOG("encode failed type=0x%02x", type); return; }
    if (!lk_push(enc, enc_len)) {
        LOG("link TX full, drop ctrl type=0x%02x", type);
        return;
    }
    lk_arm_epollout();
}

// ── enqueue_data: DATA frames — assigns seq, saves to ARQ slot for retransmit
// Returns false if ARQ window full (caller should stop reading TCP).
static bool enqueue_data(uint8_t conn_id, const uint8_t *payload, size_t plen) {
    if ((uint8_t)(g_tx_seq - g_tx_base) >= ARQ_WINDOW) {
        arq_pause_conns();
        return false;
    }
    uint8_t     seq  = g_tx_seq;
    arq_slot_t *slot = &g_arq[seq % ARQ_WINDOW];

    slot->enc_len = frame_encode(TB_DATA, conn_id, seq, payload, plen,
                                  slot->enc, sizeof(slot->enc));
    if (!slot->enc_len) { LOG("encode failed TB_DATA conn=%u", conn_id); return false; }

    slot->used       = true;
    slot->sent_at_ms = now_ms();
    g_tx_seq++;

    if (!lk_push(slot->enc, slot->enc_len))
        LOG("arq: TX ring full, seq=%u will retransmit on timeout", seq);
    else
        lk_arm_epollout();

    if ((uint8_t)(g_tx_seq - g_tx_base) >= ARQ_WINDOW)
        arq_pause_conns();
    return true;
}

// ── connection close ──────────────────────────────────────────────────────────
static void conn_close(int id, bool send_close) {
    conn_t *c = &g_conns[id];
    if (c->fd < 0) return;
    ep_del(c->fd);
    close(c->fd);
    c->fd = -1;
    c->epev = 0;
    c->paused = false;
    c->flow_paused = false;
    c->pause_sent = false;
    c->arq_paused = false;
    c->tx_head = c->tx_tail = 0;
    if (send_close) enqueue_ctrl(TB_CLOSE, (uint8_t)id, 0, NULL, 0);
    LOG("conn %d closed", id);
}

static void close_all_conns(bool send_close) {
    for (int i = 0; i < MAX_CONNS; i++)
        if (g_conns[i].fd >= 0) conn_close(i, send_close);
}

// ── TCP connect (Pi forwarder mode) ──────────────────────────────────────────
static int tcp_connect_to_target(void) {
    struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM };
    struct addrinfo *res = NULL;
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", g_fwd_port);
    if (getaddrinfo(g_fwd_host, port_str, &hints, &res) != 0 || !res) return -1;

    int fd = -1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC,
                    ai->ai_protocol);
        if (fd < 0) continue;

        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0 || errno == EINPROGRESS)
            break;

        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

// ── link frame dispatch ───────────────────────────────────────────────────────
static void dispatch_frame(const uint8_t *enc, size_t enc_len, int64_t now) {
    static uint8_t dec[FRAME_DEC_MAX];
    size_t dec_len = 0;

    if (enc_len > (size_t)(FRAME_ENC_MAX + 1)) {
        LOG("frame merge: enc_len=%zu (max=%d) — NAK seq=%u",
            enc_len, FRAME_ENC_MAX + 1, g_rx_next);
        if (g_link.up) enqueue_ctrl(TB_NAK, 0, g_rx_next, NULL, 0);
        return;
    }
    cobs_ret_t cr = cobs_decode(enc, enc_len, dec, sizeof(dec), &dec_len);
    if (cr != COBS_RET_SUCCESS) {
        LOG("COBS decode failed enc_len=%zu err=%s first=0x%02x — NAK seq=%u",
            enc_len,
            cr == COBS_RET_ERR_BAD_PAYLOAD ? "BAD_PAYLOAD" :
            cr == COBS_RET_ERR_EXHAUSTED   ? "EXHAUSTED"   : "BAD_ARG",
            enc[0], g_rx_next);
        if (g_link.up) enqueue_ctrl(TB_NAK, 0, g_rx_next, NULL, 0);
        return;
    }
    if (dec_len < 7) return;  // min: type + conn_id + seq + crc32

    uint8_t        type    = dec[0];
    uint8_t        id      = dec[1];
    uint8_t        seq     = dec[2];
    size_t         plen    = dec_len - 7;
    const uint8_t *payload = dec + 3;

    uint32_t got = (uint32_t)dec[dec_len-4] | (uint32_t)dec[dec_len-3]<<8
                 | (uint32_t)dec[dec_len-2]<<16 | (uint32_t)dec[dec_len-1]<<24;
    if (crc32(dec, 3 + plen) != got) {
        LOG("CRC mismatch enc_len=%zu — NAK seq=%u", enc_len, g_rx_next);
        if (g_link.up) enqueue_ctrl(TB_NAK, 0, g_rx_next, NULL, 0);
        return;
    }

    switch (type) {
    case TB_HELLO:
        if (!g_link.up) {
            g_link.up = true;
            LOG("link up");
            enqueue_ctrl(TB_HELLO, 0, 0, NULL, 0);
        }
        return;
    case TB_PING:
        enqueue_ctrl(TB_PONG, 0, 0, NULL, 0);
        return;
    case TB_PONG:
        return;

    case TB_ACK:
        arq_ack(seq);
        return;

    case TB_NAK:
        arq_nak(seq, now);
        return;

    case TB_OPEN:
        if (g_is_listener) return;
        if (id >= MAX_CONNS) { enqueue_ctrl(TB_CLOSE, id, 0, NULL, 0); return; }
        if (g_conns[id].fd >= 0) conn_close(id, false);
        {
            int fd = tcp_connect_to_target();
            if (fd < 0) {
                LOG("conn %d: connect to %s:%d failed: %s",
                    id, g_fwd_host, g_fwd_port, strerror(errno));
                enqueue_ctrl(TB_CLOSE, id, 0, NULL, 0);
                return;
            }
            g_conns[id].fd        = fd;
            g_conns[id].epev      = 0;
            g_conns[id].paused    = g_link.paused;
            g_conns[id].arq_paused = g_arq_paused;
            conn_epoll_update(&g_conns[id]);
            LOG("conn %d: connected to %s:%d", id, g_fwd_host, g_fwd_port);
        }
        return;

    case TB_DATA:
        // ARQ sequence check
        if (seq != g_rx_next) {
            uint8_t ahead = (uint8_t)(seq - g_rx_next);
            if (ahead < 128u) {
                // seq is genuinely ahead — gap; request the missing frame
                LOG("TB_DATA gap seq=%u expected=%u", seq, g_rx_next);
                enqueue_ctrl(TB_NAK, 0, g_rx_next, NULL, 0);
            } else {
                // seq is behind — retransmit of already-acked frame; re-ACK
                enqueue_ctrl(TB_ACK, 0, seq, NULL, 0);
            }
            return;
        }
        if (id >= MAX_CONNS) {
            LOG("TB_DATA bad id=%u drop %zu bytes", id, plen);
            g_rx_next++;
            enqueue_ctrl(TB_ACK, 0, seq, NULL, 0);
            return;
        }

        if (g_conns[id].fd < 0) {
            LOG("TB_DATA conn %u closed drop %zu bytes", id, plen);
            g_rx_next++;
            enqueue_ctrl(TB_ACK, 0, seq, NULL, 0);
            enqueue_ctrl(TB_CLOSE, id, 0, NULL, 0);
            return;
        }
        if (plen == 0) {
            g_rx_next++;
            enqueue_ctrl(TB_ACK, 0, seq, NULL, 0);
            return;
        }
        conn_drain(&g_conns[id]);
        if (conn_space(&g_conns[id]) < plen) {
            LOG("conn %d txbuf full, NAK seq=%u", id, seq);
            enqueue_ctrl(TB_PAUSE, id, 0, NULL, 0);
            g_conns[id].pause_sent = true;
            enqueue_ctrl(TB_NAK, 0, g_rx_next, NULL, 0);
            return;
        }
        conn_push(&g_conns[id], payload, plen);
        g_rx_next++;
        enqueue_ctrl(TB_ACK, 0, seq, NULL, 0);
        conn_drain(&g_conns[id]);
        conn_epoll_update(&g_conns[id]);
        {
            conn_t *c = &g_conns[id];
            uint32_t avail = conn_avail(c);
            if (!c->pause_sent && avail > CONN_HIGH_WATER) {
                enqueue_ctrl(TB_PAUSE, id, 0, NULL, 0);
                c->pause_sent = true;
            } else if (c->pause_sent && avail < CONN_LOW_WATER) {
                enqueue_ctrl(TB_RESUME, id, 0, NULL, 0);
                c->pause_sent = false;
            }
        }
        return;

    case TB_CLOSE:
        if (id < MAX_CONNS) conn_close(id, false);
        return;

    case TB_PAUSE:
        if (id >= MAX_CONNS || g_conns[id].fd < 0) return;
        g_conns[id].flow_paused = true;
        conn_epoll_update(&g_conns[id]);
        return;

    case TB_RESUME:
        if (id >= MAX_CONNS || g_conns[id].fd < 0) return;
        g_conns[id].flow_paused = false;
        conn_epoll_update(&g_conns[id]);
        return;

    default: return;
    }
}

// ── link RX parse ─────────────────────────────────────────────────────────────
static void link_parse_rx(int64_t now) {
    link_t *lk = &g_link;
    while (true) {
        uint8_t *delim = memchr(lk->rxbuf, COBS_FRAME_DELIMITER, lk->rxbuf_len);
        if (!delim) break;
        size_t flen = (size_t)(delim - lk->rxbuf);
        if (flen > 0) dispatch_frame(lk->rxbuf, flen + 1, now);
        size_t consumed = flen + 1;
        lk->rxbuf_len -= consumed;
        memmove(lk->rxbuf, lk->rxbuf + consumed, lk->rxbuf_len);
    }
    if (lk->rxbuf_len >= sizeof(lk->rxbuf)) {
        LOG("link RX overflow, discarding %zu bytes", lk->rxbuf_len);
        lk->rxbuf_len = 0;
    }
}

// ── link open/close ───────────────────────────────────────────────────────────
static void link_close(int64_t now) {
    link_t *lk = &g_link;
    if (lk->fd >= 0) {
        ep_del(lk->fd);
        close(lk->fd);
        lk->fd = -1;
        lk->epev = 0;
    }
    if (lk->up) {
        lk->up = false;
        LOG("link down");
        close_all_conns(false);
    }
    lk->reconnect_at = now + lk->backoff_ms;
    lk->backoff_ms *= 2;
    if (lk->backoff_ms > RECONNECT_MAX) lk->backoff_ms = RECONNECT_MAX;
    // Reset ARQ on link drop
    for (int i = 0; i < ARQ_WINDOW; i++) g_arq[i].used = false;
    g_tx_seq = g_tx_base = g_rx_next = 0;
    g_arq_paused = false;
}

static void link_try_open(int64_t now) {
    link_t *lk = &g_link;
    if (lk->fd >= 0) return;

    int fd = open(lk->dev, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) return;

    struct termios t;
    tcgetattr(fd, &t);
    cfmakeraw(&t);
    t.c_iflag |= IGNBRK | IGNPAR;
    t.c_cflag |= CLOCAL | CREAD;
    tcsetattr(fd, TCSANOW, &t);

    lk->fd = fd;
    lk->rxbuf_len = 0;
    lk->tx_head = lk->tx_tail = 0;
    lk->last_rx_ms = lk->last_tx_ms = now;
    lk->up = false;
    lk->paused = false;
    lk->backoff_ms = RECONNECT_MIN;
    lk->epev = EPOLLIN;
    ep_set(fd, EPOLLIN, &g_link_tag);

    LOG("link opened: %s", lk->dev);
    enqueue_ctrl(TB_HELLO, 0, 0, NULL, 0);
}

// ── K1C accept new TCP connection ─────────────────────────────────────────────
static void listener_accept(void) {
    struct sockaddr_storage sa;
    socklen_t sl = sizeof(sa);
    int fd = accept4(g_listen_fd, (struct sockaddr *)&sa, &sl,
                     SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd < 0) return;

    int id = -1;
    for (int i = 0; i < MAX_CONNS; i++)
        if (g_conns[i].fd < 0) { id = i; break; }
    if (id < 0) { LOG("all slots full, rejecting"); close(fd); return; }
    if (!g_link.up) { LOG("link not up, rejecting"); close(fd); return; }

    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    g_conns[id].fd        = fd;
    g_conns[id].epev      = 0;
    g_conns[id].paused    = g_link.paused;
    g_conns[id].arq_paused = g_arq_paused;
    conn_epoll_update(&g_conns[id]);
    enqueue_ctrl(TB_OPEN, (uint8_t)id, 0, NULL, 0);
    LOG("conn %d: accepted", id);
}

// ── conn readable: read TCP → send TB_DATA ────────────────────────────────────
static void conn_on_readable(int id) {
    conn_t *c = &g_conns[id];
    if (lk_avail() >= LINK_HIGH_WATER) { pause_all_conns(); return; }
    if ((uint8_t)(g_tx_seq - g_tx_base) >= ARQ_WINDOW) { arq_pause_conns(); return; }
    static uint8_t buf[MAX_PAYLOAD];
    ssize_t n = read(c->fd, buf, sizeof(buf));
    if (n <= 0) {
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) return;
        conn_close(id, true);
        return;
    }
    size_t off = 0;
    while (off < (size_t)n) {
        size_t chunk = (size_t)n - off;
        if (chunk > MAX_PAYLOAD) chunk = MAX_PAYLOAD;
        if (!enqueue_data((uint8_t)id, buf + off, chunk)) break;
        off += chunk;
    }
    if (lk_avail() > LINK_HIGH_WATER) pause_all_conns();
}

// ── main event loop ───────────────────────────────────────────────────────────
#define MAX_EVENTS 32

static void run(void) {
    g_epfd = epoll_create1(EPOLL_CLOEXEC);
    if (g_epfd < 0) DIE("epoll_create1: %s", strerror(errno));

    for (int i = 0; i < MAX_CONNS; i++) g_conns[i].fd = -1;

    if (g_is_listener) {
        int lfd = socket(AF_INET6, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (lfd < 0)
            lfd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (lfd < 0) DIE("listen socket: %s", strerror(errno));

        int one = 1;
        setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

        struct sockaddr_in6 sa6 = { .sin6_family = AF_INET6,
                                    .sin6_port   = htons((uint16_t)g_fwd_port) };
        struct sockaddr_in  sa4 = { .sin_family  = AF_INET,
                                    .sin_port    = htons((uint16_t)g_fwd_port) };
        if (strcmp(g_fwd_host, "0.0.0.0") == 0 || strcmp(g_fwd_host, "") == 0) {
            sa6.sin6_addr = in6addr_any;
            if (bind(lfd, (struct sockaddr *)&sa6, sizeof(sa6)) < 0) {
                close(lfd);
                lfd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
                setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
                sa4.sin_addr.s_addr = INADDR_ANY;
                if (bind(lfd, (struct sockaddr *)&sa4, sizeof(sa4)) < 0)
                    DIE("bind: %s", strerror(errno));
            }
        } else if (inet_pton(AF_INET6, g_fwd_host, &sa6.sin6_addr) == 1) {
            if (bind(lfd, (struct sockaddr *)&sa6, sizeof(sa6)) < 0)
                DIE("bind: %s", strerror(errno));
        } else {
            inet_pton(AF_INET, g_fwd_host, &sa4.sin_addr);
            close(lfd);
            lfd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
            setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
            if (bind(lfd, (struct sockaddr *)&sa4, sizeof(sa4)) < 0)
                DIE("bind: %s", strerror(errno));
        }
        if (listen(lfd, 16) < 0) DIE("listen: %s", strerror(errno));
        g_listen_fd = lfd;
        ep_set(g_listen_fd, EPOLLIN, &g_listen_tag);
        LOG("listening on port %d", g_fwd_port);
    }

    int64_t now = now_ms();
    link_try_open(now);

    for (;;) {
        now = now_ms();

        int64_t dl = INT64_MAX;
        if (g_link.fd < 0) {
            dl = g_link.reconnect_at;
        } else if (g_link.up) {
            dl = g_link.last_tx_ms + PING_IDLE_MS;
            // Wake for ARQ timeout
            if ((uint8_t)(g_tx_seq - g_tx_base) > 0) {
                arq_slot_t *oldest = &g_arq[g_tx_base % ARQ_WINDOW];
                if (oldest->used) {
                    int64_t arq_dl = oldest->sent_at_ms + ARQ_TIMEOUT_MS;
                    if (arq_dl < dl) dl = arq_dl;
                }
            }
        }

        int timeout = 5000;
        if (dl != INT64_MAX) {
            int64_t t = dl - now;
            timeout = (t <= 0) ? 0 : (t < 5000 ? (int)t : 5000);
        }

        struct epoll_event evs[MAX_EVENTS];
        int n = epoll_wait(g_epfd, evs, MAX_EVENTS, timeout);
        if (n < 0) { if (errno == EINTR) continue; DIE("epoll_wait: %s", strerror(errno)); }

        now = now_ms();

        for (int i = 0; i < n; i++) {
            void    *ptr = evs[i].data.ptr;
            uint32_t ev  = evs[i].events;

            if (ptr == &g_link_tag) {
                if (ev & (EPOLLERR | EPOLLHUP)) { link_close(now); continue; }
                if (ev & EPOLLIN) {
                    link_t *lk = &g_link;
                    size_t space = sizeof(lk->rxbuf) - lk->rxbuf_len;
                    if (!space) { lk->rxbuf_len = 0; space = sizeof(lk->rxbuf); }
                    ssize_t r = read(lk->fd, lk->rxbuf + lk->rxbuf_len, space);
                    if (r <= 0) {
                        if (r < 0 && (errno == EAGAIN || errno == EINTR)) goto skip_rx;
                        link_close(now); continue;
                    }
                    lk->last_rx_ms = now;
                    lk->rxbuf_len += (size_t)r;
                    link_parse_rx(now);
                }
                skip_rx:
                if (ev & EPOLLOUT) lk_drain(now);
            } else if (ptr == &g_listen_tag) {
                listener_accept();
            } else {
                conn_t *c  = (conn_t *)ptr;
                int     id = (int)(c - g_conns);
                if (ev & EPOLLERR) { conn_close(id, true); continue; }
                if (ev & EPOLLIN)  conn_on_readable(id);
                if (c->fd >= 0 && (ev & EPOLLOUT)) {
                    conn_drain(c);
                    if (c->pause_sent && conn_avail(c) < CONN_LOW_WATER) {
                        enqueue_ctrl(TB_RESUME, (uint8_t)id, 0, NULL, 0);
                        c->pause_sent = false;
                    }
                    conn_epoll_update(c);
                }
                if (c->fd >= 0 && (ev & EPOLLHUP)) conn_close(id, true);
            }
        }

        now = now_ms();

        if (g_link.fd < 0) {
            if (now >= g_link.reconnect_at) link_try_open(now);
        } else if (g_link.up) {
            if ((now - g_link.last_tx_ms) > PING_IDLE_MS)
                enqueue_ctrl(TB_PING, 0, 0, NULL, 0);
            if ((now - g_link.last_rx_ms) > LINK_DEAD_MS) {
                LOG("link RX timeout");
                link_close(now);
            } else {
                arq_tick(now);
            }
        } else {
            if ((now - g_link.last_tx_ms) > 2000)
                enqueue_ctrl(TB_HELLO, 0, 0, NULL, 0);
        }

        if (!g_link.paused && lk_avail() > LINK_HIGH_WATER)
            pause_all_conns();
        else if (g_link.paused && lk_avail() < LINK_LOW_WATER)
            resume_all_conns();
    }
}

// ── main ──────────────────────────────────────────────────────────────────────
static void usage(const char *prog) {
    fprintf(stderr,
        "Usage:\n"
        "  %s SERIAL_DEV listen  BIND_ADDR:PORT\n"
        "  %s SERIAL_DEV forward TARGET_HOST:PORT\n",
        prog, prog);
    exit(1);
}

int main(int argc, char **argv) {
    if (argc != 4) usage(argv[0]);

    strncpy(g_link.dev, argv[1], sizeof(g_link.dev) - 1);
    g_link.fd = -1;
    g_link.backoff_ms = RECONNECT_MIN;

    const char *mode = argv[2];
    if (strcmp(mode, "listen") == 0)
        g_is_listener = true;
    else if (strcmp(mode, "forward") == 0)
        g_is_listener = false;
    else
        usage(argv[0]);

    const char *hostport = argv[3];
    const char *colon = strrchr(hostport, ':');
    if (!colon) usage(argv[0]);
    size_t hlen = (size_t)(colon - hostport);
    if (hlen >= sizeof(g_fwd_host)) usage(argv[0]);
    memcpy(g_fwd_host, hostport, hlen);
    g_fwd_host[hlen] = '\0';
    if (!parse_port(colon + 1, &g_fwd_port)) usage(argv[0]);

    crc32_init();
    LOG("tcpbridge %s %s %s:%d",
        g_link.dev, g_is_listener ? "listen" : "forward",
        g_fwd_host, g_fwd_port);
    run();
    return 0;
}
