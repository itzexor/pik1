// src/tcpbridge.c — multi-connection TCP-over-serial bridge
// Wire protocol: COBS + CRC32, frame layout:
//   [type:1][conn_id:1][seq_le:2][payload][crc32_le:4]
// Sequencing is detection-only: any reliable-frame gap is treated as link failure.
// Runs on ttyGS2 (K1C) / ttyACM2 (Pi).

#include "nanocobs/cobs.h"
#include "fd.h"
#include "frame.h"
#include "logging.h"
#include "tty.h"
#include "util.h"
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
#include <unistd.h>

// ── frame types ───────────────────────────────────────────────────────────────
#define TB_OPEN   0x20u
#define TB_DATA   0x21u
#define TB_CLOSE  0x22u
#define TB_PAUSE  0x23u
#define TB_RESUME 0x24u

// ── sizing / pacing ───────────────────────────────────────────────────────────
#define MAX_CONNS         16
#define MAX_PAYLOAD       2032
#define FRAME_DEC_MAX     (4 + MAX_PAYLOAD + 4)  // type + conn + seq16 + payload + crc32
#define FRAME_ENC_MAX     COBS_ENCODE_MAX(FRAME_DEC_MAX)

#define CONN_RING_CAP     (1u << 18)             // 256 KB per connection
#define CONN_RING_MASK    (CONN_RING_CAP - 1u)
#define CONN_HIGH_WATER   (64u  * 1024u)
#define CONN_LOW_WATER    (16u  * 1024u)

#define LINK_TXQ_CAP      64u                     // ~128 KB of encoded frames
#define LINK_TX_HIGH      (32u  * 1024u)
#define LINK_TX_LOW       (8u   * 1024u)
#define LINK_RX_CAP       (1u << 17)              // 128 KB serial RX staging
#define LINK_WRITE_BUDGET 4096u
#define LINK_TX_RATE_BPS  6000000u
#define LINK_TX_BURST     4096u

#define RECONNECT_MIN     500
#define RECONNECT_MAX     8000

// ── types ─────────────────────────────────────────────────────────────────────
typedef struct {
    int      fd;
    uint32_t epev;
    uint8_t  txbuf[CONN_RING_CAP];
    uint32_t tx_head, tx_tail;
    bool     paused;       // local serial TX queue high-water
    bool     flow_paused;  // remote sent TB_PAUSE
    bool     pause_sent;   // we sent TB_PAUSE
} conn_t;

typedef struct {
    uint8_t  enc[FRAME_ENC_MAX + 1];
    size_t   len;
    size_t   pos;
} tx_frame_t;

typedef struct {
    int      fd;
    bool     up;
    bool     paused;
    uint32_t epev;

    tx_frame_t txq[LINK_TXQ_CAP];
    uint32_t   tx_head, tx_tail, tx_count;
    uint32_t   tx_bytes;
    uint32_t   tx_tokens;
    int64_t    tx_token_ms;

    uint8_t  rxbuf[LINK_RX_CAP];
    size_t   rxbuf_len;

    uint16_t tx_seq;
    uint16_t rx_seq;

    int64_t  last_tx_ms;
    int64_t  last_rx_ms;
    int64_t  reconnect_at;
    int      backoff_ms;
    char     dev[128];
} link_t;

// ── globals ───────────────────────────────────────────────────────────────────
static conn_t g_conns[MAX_CONNS];
static link_t g_link;
static int    g_listen_fd = -1;
static int    g_epfd      = -1;
static bool   g_is_listener;
static char   g_fwd_host[64];
static int    g_fwd_port;

static int g_link_tag;
static int g_listen_tag;

static uint64_t g_rx_frames;
static uint64_t g_tx_frames;
static uint64_t g_rx_reads;
static uint64_t g_rx_bytes;
static uint64_t g_tx_writes;
static uint64_t g_tx_bytes;

// ── logging ───────────────────────────────────────────────────────────────────
static void log_msg(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fputs("[tcp] ", stderr);
    vfprintf(stderr, fmt, ap); va_end(ap);
    fputc('\n', stderr);
}
#define LOG(...) log_msg(__VA_ARGS__)
#define DIE(...) do { log_msg(__VA_ARGS__); exit(1); } while (0)

// ── forward decls ─────────────────────────────────────────────────────────────
static void link_close(int64_t now);
static bool enqueue_frame(uint8_t type, uint8_t conn_id,
                          const uint8_t *payload, size_t plen);
static bool dispatch_rx_frame(void *ctx, const uint8_t *enc, size_t enc_len);

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
    uint32_t want = ((!c->paused && !c->flow_paused) ? EPOLLIN : 0u)
                  | (conn_avail(c) ? EPOLLOUT : 0u);
    if (want == c->epev) return;
    c->epev = want;
    if (want) pik_epoll_set(g_epfd, c->fd, want, c);
    else      pik_epoll_del(g_epfd, c->fd);
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

// ── link TX frame queue / pacing ──────────────────────────────────────────────
static bool frame_is_reliable(uint8_t type) {
    return type == TB_OPEN || type == TB_DATA || type == TB_CLOSE ||
           type == TB_PAUSE || type == TB_RESUME;
}

static void lk_refill_tokens(int64_t now) {
    link_t *lk = &g_link;
    if (!lk->tx_token_ms) lk->tx_token_ms = now;
    int64_t elapsed = now - lk->tx_token_ms;
    if (elapsed <= 0) return;
    uint64_t add = (uint64_t)elapsed * LINK_TX_RATE_BPS / 1000u;
    if (!add) return;
    uint64_t tokens = (uint64_t)lk->tx_tokens + add;
    lk->tx_tokens = tokens > LINK_TX_BURST ? LINK_TX_BURST : (uint32_t)tokens;
    lk->tx_token_ms = now;
}

static void lk_update_epoll(void) {
    link_t *lk = &g_link;
    if (lk->fd < 0) return;
    uint32_t want = EPOLLIN | ((lk->tx_count && lk->tx_tokens) ? EPOLLOUT : 0u);
    if (want != lk->epev) {
        lk->epev = want;
        pik_epoll_set(g_epfd, lk->fd, want, &g_link_tag);
    }
}

static bool lk_queue_encoded(const uint8_t *enc, size_t enc_len) {
    link_t *lk = &g_link;
    if (lk->tx_count >= LINK_TXQ_CAP) return false;
    tx_frame_t *f = &lk->txq[lk->tx_tail];
    memcpy(f->enc, enc, enc_len);
    f->len = enc_len;
    f->pos = 0;
    lk->tx_tail = (lk->tx_tail + 1u) % LINK_TXQ_CAP;
    lk->tx_count++;
    lk->tx_bytes += (uint32_t)enc_len;
    g_tx_frames++;
    lk_update_epoll();
    return true;
}

static void lk_drain(int64_t now) {
    link_t *lk = &g_link;
    lk_refill_tokens(now);

    uint32_t budget = LINK_WRITE_BUDGET;
    if (budget > lk->tx_tokens) budget = lk->tx_tokens;

    while (lk->tx_count && budget) {
        tx_frame_t *f = &lk->txq[lk->tx_head];
        size_t n = f->len - f->pos;
        if (n > budget) n = budget;
        ssize_t w = write(lk->fd, f->enc + f->pos, n);
        if (w <= 0) {
            if (w < 0 && errno != EAGAIN && errno != EINTR) {
                LOG("link write: %s", strerror(errno));
                link_close(now);
            }
            break;
        }
        f->pos += (size_t)w;
        lk->tx_tokens -= (uint32_t)w;
        budget -= (uint32_t)w;
        lk->last_tx_ms = now;
        g_tx_writes++;
        g_tx_bytes += (uint64_t)w;

        if (f->pos == f->len) {
            lk->tx_bytes -= (uint32_t)f->len;
            lk->tx_head = (lk->tx_head + 1u) % LINK_TXQ_CAP;
            lk->tx_count--;
        }
    }
    lk_update_epoll();
}

// ── frame encode / enqueue ────────────────────────────────────────────────────
static bool enqueue_frame(uint8_t type, uint8_t conn_id,
                          const uint8_t *payload, size_t plen) {
    link_t *lk = &g_link;
    if (plen > MAX_PAYLOAD) return false;
    if (!lk->up && lk->fd >= 0) return true;

    static uint8_t dec[FRAME_DEC_MAX];
    static uint8_t enc[FRAME_ENC_MAX + 1];
    uint8_t header[4];

    bool reliable = frame_is_reliable(type);
    uint16_t seq = reliable ? lk->tx_seq : 0;

    header[0] = type;
    header[1] = conn_id;
    header[2] = (uint8_t)seq;
    header[3] = (uint8_t)(seq >> 8);

    size_t enc_len = 0;
    if (pik_frame_encode(header, sizeof(header), payload, plen,
                         dec, sizeof(dec), enc, sizeof(enc), &enc_len) != PIK_FRAME_OK) {
        LOG("encode failed type=0x%02x", type);
        return false;
    }
    if (!lk_queue_encoded(enc, enc_len)) {
        LOG("link TX frame queue full, drop type=0x%02x", type);
        return false;
    }
    if (reliable)
        lk->tx_seq++;
    return true;
}

// ── connection close ──────────────────────────────────────────────────────────
static void conn_close(int id, bool send_close) {
    conn_t *c = &g_conns[id];
    if (c->fd < 0) return;
    pik_epoll_del(g_epfd, c->fd);
    close(c->fd);
    c->fd = -1;
    c->epev = 0;
    c->paused = false;
    c->flow_paused = false;
    c->pause_sent = false;
    c->tx_head = c->tx_tail = 0;
    if (send_close && !enqueue_frame(TB_CLOSE, (uint8_t)id, NULL, 0))
        LOG("conn %d: unable to queue CLOSE", id);
}

static void close_all_conns(bool send_close) {
    for (int i = 0; i < MAX_CONNS; i++)
        if (g_conns[i].fd >= 0) conn_close(i, send_close);
}

// ── fatal link corruption diagnostics ─────────────────────────────────────────
static void link_fail_frame(const char *reason, const uint8_t *enc, size_t enc_len,
                            int64_t now) {
    LOG("link failure: %s enc_len=%zu first=0x%02x rx_frames=%llu tx_frames=%llu rx_reads=%llu rx_bytes=%llu tx_writes=%llu tx_bytes=%llu",
        reason, enc_len, enc_len ? enc[0] : 0,
        (unsigned long long)g_rx_frames,
        (unsigned long long)g_tx_frames,
        (unsigned long long)g_rx_reads,
        (unsigned long long)g_rx_bytes,
        (unsigned long long)g_tx_writes,
        (unsigned long long)g_tx_bytes);
    size_t head = enc_len < 64 ? enc_len : 64;
    pik_log_hex_sample(log_msg, "badframe head", enc, head);
    if (enc_len > head) {
        LOG("badframe tail starts at +%zu", enc_len - head);
        pik_log_hex_sample(log_msg, "badframe tail", enc + enc_len - head, head);
    }
    link_close(now);
}

static void link_fail_text(const char *reason, int64_t now) {
    LOG("link failure: %s rx_frames=%llu tx_frames=%llu rx_reads=%llu rx_bytes=%llu",
        reason,
        (unsigned long long)g_rx_frames,
        (unsigned long long)g_tx_frames,
        (unsigned long long)g_rx_reads,
        (unsigned long long)g_rx_bytes);
    link_close(now);
}

// ── TCP connect (Pi forwarder mode) ───────────────────────────────────────────
static int tcp_connect_to_target(void) {
    struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM };
    struct addrinfo *res = NULL;
    char port[8];
    snprintf(port, sizeof(port), "%d", g_fwd_port);
    if (getaddrinfo(g_fwd_host, port, &hints, &res) != 0 || !res) return -1;

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

// ── frame dispatch ────────────────────────────────────────────────────────────
static bool dispatch_frame(const uint8_t *enc, size_t enc_len, int64_t now) {
    static uint8_t dec[FRAME_DEC_MAX];
    pik_frame_t frame;
    g_rx_frames++;

    pik_frame_status_t st = pik_frame_decode(enc, enc_len, FRAME_ENC_MAX + 1,
                                             4, dec, sizeof(dec), &frame);
    if (st != PIK_FRAME_OK) {
        link_fail_frame(pik_frame_status_text(st), enc, enc_len, now);
        return false;
    }

    uint8_t        type    = frame.header[0];
    uint8_t        id      = frame.header[1];
    uint16_t       seq     = (uint16_t)frame.header[2] | (uint16_t)frame.header[3] << 8;
    size_t         plen    = frame.payload_len;
    const uint8_t *payload = frame.payload;

    if (frame_is_reliable(type)) {
        if (seq != g_link.rx_seq) {
            LOG("link failure: seq gap type=0x%02x seq=%u expected=%u",
                type, seq, g_link.rx_seq);
            link_close(now);
            return false;
        }
        g_link.rx_seq++;
    }

    switch (type) {
    case TB_OPEN:
        if (g_is_listener || id >= MAX_CONNS) {
            enqueue_frame(TB_CLOSE, id, NULL, 0);
            return true;
        }
        if (g_conns[id].fd >= 0) conn_close(id, false);
        {
            int fd = tcp_connect_to_target();
            if (fd < 0) {
                LOG("conn %d: connect to %s:%d failed: %s",
                    id, g_fwd_host, g_fwd_port, strerror(errno));
                enqueue_frame(TB_CLOSE, id, NULL, 0);
                return true;
            }
            g_conns[id].fd = fd;
            g_conns[id].epev = 0;
            g_conns[id].paused = g_link.paused;
            g_conns[id].flow_paused = false;
            g_conns[id].pause_sent = false;
            conn_epoll_update(&g_conns[id]);
        }
        return true;

    case TB_DATA:
        if (id >= MAX_CONNS) return true;
        if (g_conns[id].fd < 0) {
            enqueue_frame(TB_CLOSE, id, NULL, 0);
            return true;
        }
        if (!plen) return true;
        conn_drain(&g_conns[id]);
        if (conn_space(&g_conns[id]) < plen) {
            if (!g_conns[id].pause_sent && enqueue_frame(TB_PAUSE, id, NULL, 0))
                g_conns[id].pause_sent = true;
            link_fail_text("conn output buffer overflow despite PAUSE", now);
            return false;
        }
        conn_push(&g_conns[id], payload, plen);
        conn_drain(&g_conns[id]);
        {
            conn_t *c = &g_conns[id];
            uint32_t avail = conn_avail(c);
            if (!c->pause_sent && avail > CONN_HIGH_WATER) {
                if (enqueue_frame(TB_PAUSE, id, NULL, 0))
                    c->pause_sent = true;
            } else if (c->pause_sent && avail < CONN_LOW_WATER) {
                if (enqueue_frame(TB_RESUME, id, NULL, 0))
                    c->pause_sent = false;
            }
        }
        conn_epoll_update(&g_conns[id]);
        return true;

    case TB_CLOSE:
        if (id < MAX_CONNS) conn_close(id, false);
        return true;

    case TB_PAUSE:
        if (id < MAX_CONNS && g_conns[id].fd >= 0) {
            g_conns[id].flow_paused = true;
            conn_epoll_update(&g_conns[id]);
        }
        return true;

    case TB_RESUME:
        if (id < MAX_CONNS && g_conns[id].fd >= 0) {
            g_conns[id].flow_paused = false;
            conn_epoll_update(&g_conns[id]);
        }
        return true;

    default:
        return true;
    }
}

// ── link RX ───────────────────────────────────────────────────────────────────
static bool link_parse_rx(int64_t now) {
    link_t *lk = &g_link;
    pik_frame_status_t st = pik_frame_rx_consume(lk->rxbuf, &lk->rxbuf_len,
                                                 sizeof(lk->rxbuf),
                                                 dispatch_rx_frame, &now);
    if (st == PIK_FRAME_CALLBACK_FAILED)
        return false;
    if (st == PIK_FRAME_RX_OVERFLOW) {
        link_fail_text("RX buffer full without delimiter", now);
        return false;
    }
    return true;
}

static bool dispatch_rx_frame(void *ctx, const uint8_t *enc, size_t enc_len) {
    int64_t now = *(int64_t *)ctx;
    return dispatch_frame(enc, enc_len, now);
}

static bool link_read_available(int64_t now) {
    link_t *lk = &g_link;
    while (lk->fd >= 0) {
        size_t space = sizeof(lk->rxbuf) - lk->rxbuf_len;
        if (!space) {
            if (!link_parse_rx(now)) return false;
            space = sizeof(lk->rxbuf) - lk->rxbuf_len;
            if (!space) {
                link_fail_text("RX buffer full before read", now);
                return false;
            }
        }

        ssize_t r = read(lk->fd, lk->rxbuf + lk->rxbuf_len, space);
        if (r < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN) return true;
            link_close(now);
            return false;
        }
        if (r == 0) return true;

        lk->last_rx_ms = now;
        g_rx_reads++;
        g_rx_bytes += (uint64_t)r;
        lk->rxbuf_len += (size_t)r;
        if (!link_parse_rx(now)) return false;
        if (lk->tx_count) lk_drain(now);
    }
    return false;
}

// ── link open/close ───────────────────────────────────────────────────────────
static void link_close(int64_t now) {
    link_t *lk = &g_link;
    if (lk->fd >= 0) {
        pik_epoll_del(g_epfd, lk->fd);
        close(lk->fd);
        lk->fd = -1;
        lk->epev = 0;
    }
    if (lk->up) {
        lk->up = false;
        LOG("link down");
    }
    close_all_conns(false);
    lk->rxbuf_len = 0;
    lk->tx_head = lk->tx_tail = lk->tx_count = lk->tx_bytes = 0;
    lk->tx_seq = lk->rx_seq = 0;
    lk->tx_tokens = LINK_TX_BURST;
    lk->tx_token_ms = now;
    lk->paused = false;

    lk->reconnect_at = now + pik_backoff_next(&lk->backoff_ms, RECONNECT_MAX);
}

static void link_try_open(int64_t now) {
    link_t *lk = &g_link;
    if (lk->fd >= 0) return;

    int fd = open(lk->dev, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        lk->reconnect_at = now + pik_backoff_next(&lk->backoff_ms, RECONNECT_MAX);
        return;
    }

    if (tty_set_byte_raw(fd) < 0) {
        LOG("termios setup failed on %s: %s", lk->dev, strerror(errno));
        close(fd);
        lk->reconnect_at = now + pik_backoff_next(&lk->backoff_ms, RECONNECT_MAX);
        return;
    }

    lk->fd = fd;
    lk->rxbuf_len = 0;
    lk->tx_head = lk->tx_tail = lk->tx_count = lk->tx_bytes = 0;
    lk->tx_seq = lk->rx_seq = 0;
    lk->last_rx_ms = lk->last_tx_ms = now;
    lk->up = true;
    lk->paused = false;
    lk->backoff_ms = RECONNECT_MIN;
    lk->tx_tokens = LINK_TX_BURST;
    lk->tx_token_ms = now;
    lk->epev = EPOLLIN;
    pik_epoll_set(g_epfd, fd, EPOLLIN, &g_link_tag);

    LOG("link opened: %s", lk->dev);
    LOG("link up");
}

// ── listener / TCP input ──────────────────────────────────────────────────────
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

    g_conns[id].fd = fd;
    g_conns[id].epev = 0;
    g_conns[id].paused = g_link.paused;
    g_conns[id].flow_paused = false;
    g_conns[id].pause_sent = false;
    conn_epoll_update(&g_conns[id]);
    if (!enqueue_frame(TB_OPEN, (uint8_t)id, NULL, 0)) {
        LOG("conn %d: unable to queue OPEN", id);
        conn_close(id, false);
        return;
    }
}

static void conn_on_readable(int id) {
    conn_t *c = &g_conns[id];
    if (g_link.tx_bytes >= LINK_TX_HIGH || g_link.tx_count >= LINK_TXQ_CAP - 2) {
        pause_all_conns();
        return;
    }

    static uint8_t buf[MAX_PAYLOAD];
    ssize_t n = read(c->fd, buf, sizeof(buf));
    if (n <= 0) {
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) return;
        conn_close(id, true);
        return;
    }
    if (!enqueue_frame(TB_DATA, (uint8_t)id, buf, (size_t)n))
        pause_all_conns();
    if (g_link.tx_bytes > LINK_TX_HIGH || g_link.tx_count >= LINK_TXQ_CAP - 2)
        pause_all_conns();
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
                                    .sin6_port = htons((uint16_t)g_fwd_port) };
        struct sockaddr_in  sa4 = { .sin_family = AF_INET,
                                    .sin_port = htons((uint16_t)g_fwd_port) };
        if (strcmp(g_fwd_host, "0.0.0.0") == 0 || strcmp(g_fwd_host, "") == 0) {
            sa6.sin6_addr = in6addr_any;
            if (bind(lfd, (struct sockaddr *)&sa6, sizeof(sa6)) < 0) {
                close(lfd);
                lfd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
                if (lfd < 0) DIE("listen socket: %s", strerror(errno));
                setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
                sa4.sin_addr.s_addr = INADDR_ANY;
                if (bind(lfd, (struct sockaddr *)&sa4, sizeof(sa4)) < 0)
                    DIE("bind: %s", strerror(errno));
            }
        } else if (inet_pton(AF_INET6, g_fwd_host, &sa6.sin6_addr) == 1) {
            if (bind(lfd, (struct sockaddr *)&sa6, sizeof(sa6)) < 0)
                DIE("bind: %s", strerror(errno));
        } else {
            if (inet_pton(AF_INET, g_fwd_host, &sa4.sin_addr) != 1)
                DIE("bad bind address: %s", g_fwd_host);
            close(lfd);
            lfd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
            if (lfd < 0) DIE("listen socket: %s", strerror(errno));
            setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
            if (bind(lfd, (struct sockaddr *)&sa4, sizeof(sa4)) < 0)
                DIE("bind: %s", strerror(errno));
        }
        if (listen(lfd, 16) < 0) DIE("listen: %s", strerror(errno));
        g_listen_fd = lfd;
        pik_epoll_set(g_epfd, lfd, EPOLLIN, &g_listen_tag);
        LOG("listening on port %d", g_fwd_port);
    }

    int64_t now = pik_now_ms();
    link_try_open(now);

    for (;;) {
        now = pik_now_ms();
        if (g_link.fd >= 0) {
            lk_refill_tokens(now);
            if (g_link.tx_count && g_link.tx_tokens) lk_drain(now);
        }

        int64_t dl = INT64_MAX;
        if (g_link.fd < 0) {
            dl = g_link.reconnect_at;
        } else {
            if (g_link.tx_count && !g_link.tx_tokens) {
                int64_t tx_dl = now + 1;
                if (tx_dl < dl) dl = tx_dl;
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

        now = pik_now_ms();
        for (int i = 0; i < n; i++) {
            void *ptr = evs[i].data.ptr;
            uint32_t ev = evs[i].events;

            if (ptr == &g_link_tag) {
                if (ev & (EPOLLERR | EPOLLHUP)) { link_close(now); continue; }
                if (ev & EPOLLIN) {
                    if (!link_read_available(now)) continue;
                }
                if (g_link.fd >= 0 && (ev & EPOLLOUT)) lk_drain(now);
            } else if (ptr == &g_listen_tag) {
                listener_accept();
            } else {
                conn_t *c = (conn_t *)ptr;
                int id = (int)(c - g_conns);
                if (ev & EPOLLERR) { conn_close(id, true); continue; }
                if (ev & EPOLLIN) conn_on_readable(id);
                if (c->fd >= 0 && (ev & EPOLLOUT)) {
                    conn_drain(c);
                    if (c->pause_sent && conn_avail(c) < CONN_LOW_WATER) {
                        if (enqueue_frame(TB_RESUME, (uint8_t)id, NULL, 0))
                            c->pause_sent = false;
                    }
                    conn_epoll_update(c);
                }
                if (c->fd >= 0 && (ev & EPOLLHUP)) conn_close(id, true);
            }
        }

        now = pik_now_ms();
        if (g_link.fd < 0) {
            if (now >= g_link.reconnect_at) link_try_open(now);
        }

        if (!g_link.paused &&
            (g_link.tx_bytes > LINK_TX_HIGH || g_link.tx_count >= LINK_TXQ_CAP - 2))
            pause_all_conns();
        else if (g_link.paused &&
                 g_link.tx_bytes < LINK_TX_LOW && g_link.tx_count < LINK_TXQ_CAP / 2)
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
    g_link.tx_tokens = LINK_TX_BURST;

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
    if (!pik_parse_port(colon + 1, &g_fwd_port)) usage(argv[0]);

    LOG("tcpbridge %s %s %s:%d",
        g_link.dev, g_is_listener ? "listen" : "forward",
        g_fwd_host, g_fwd_port);
    run();
    return 0;
}
