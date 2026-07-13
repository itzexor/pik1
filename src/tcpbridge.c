// src/tcpbridge.c — multi-connection TCP-over-serial bridge
// Wire protocol: COBS + CRC32, frame layout:
//   [type:1][conn_id:1][session_le:4][seq_le:2][payload][crc32_le:4]
// Sessions, sequencing, bring-up grace, and bounded link-layer retransmission
// are handled by the shared link module (src/link.c); see FAILURE_MODEL.md,
// "Documented Exceptions".
// Runs on ttyGS2 (K1C) / ttyACM2 (Pi).

#include "nanocobs/cobs.h"
#include "fd.h"
#include "link.h"
#include "logging.h"
#include "tcpbridge_proto.h"
#include "util.h"
#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

// ── sizing / pacing ───────────────────────────────────────────────────────────
#define MAX_CONNS         16
#define FRAME_DEC_MAX     (PIK_TCPBRIDGE_FRAME_HEADER_LEN + PIK_TCPBRIDGE_MAX_PAYLOAD + 4)
#define FRAME_ENC_MAX     COBS_ENCODE_MAX(FRAME_DEC_MAX)

#define CONN_RING_CAP     (1u << 18)             // 256 KB per connection
#define CONN_RING_MASK    (CONN_RING_CAP - 1u)
#define CONN_HIGH_WATER   (64u  * 1024u)
#define CONN_LOW_WATER    (16u  * 1024u)

#define LINK_TX_CAP       (1u << 17)             // 128 KB encoded TX ring
#define LINK_TX_HIGH      (32u  * 1024u)
#define LINK_TX_LOW       (8u   * 1024u)
#define LINK_RX_CAP       (1u << 17)             // 128 KB serial RX staging
#define LINK_WRITE_BUDGET 4096u
#define LINK_TX_RATE_BPS  6000000u
#define LINK_TX_BURST     4096u

#define HIST_CAP          (1u << 18)             // retransmit history ring
#define HIST_SLOTS        512u

#define RECONNECT_MIN     500
#define RECONNECT_MAX     8000
#define TCP_STATUS_FD_ENV "PIK1_TCP_STATUS_FD"

// ── types ─────────────────────────────────────────────────────────────────────
typedef struct {
    int      fd;
    uint32_t epev;
    uint8_t  txbuf[CONN_RING_CAP];
    uint32_t tx_head, tx_tail;
    bool     paused;       // local serial TX queue high-water
    bool     flow_paused;  // remote sent PAUSE
    bool     pause_sent;   // we sent PAUSE
} conn_t;

// ── globals ───────────────────────────────────────────────────────────────────
static conn_t     g_conns[MAX_CONNS];
static pik_link_t g_link;
static bool       g_conns_paused;   /* link TX high-water pause of all conns */
static int64_t    g_reconnect_at;
static int        g_backoff_ms;
static char       g_dev[128];
static int        g_listen_fd = -1;
static int        g_epfd      = -1;
static bool       g_is_listener;
static char       g_fwd_host[64];
static int        g_fwd_port;
static int        g_status_fd = -1;
static bool       g_status_up;

static int g_listen_tag;

static uint8_t             s_link_tx[LINK_TX_CAP];
static uint8_t             s_link_rx[LINK_RX_CAP];
static uint8_t             s_link_hist[HIST_CAP];
static pik_link_hist_ent_t s_link_hist_ent[HIST_SLOTS];

// ── logging ───────────────────────────────────────────────────────────────────
#define LOG(...) pik_log("tcp", __VA_ARGS__)
#define DIE(...) pik_die("tcp", __VA_ARGS__)

static void status_notify(bool up) {
    if (g_status_fd < 0 || g_status_up == up) return;

    char state = up ? 'U' : 'D';
    while (write(g_status_fd, &state, 1) < 0) {
        if (errno == EINTR) continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            close(g_status_fd);
            g_status_fd = -1;
        }
        break;
    }
    g_status_up = up;
}

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
    if (g_conns_paused) return;
    g_conns_paused = true;
    for (int i = 0; i < MAX_CONNS; i++) {
        if (g_conns[i].fd < 0 || g_conns[i].paused) continue;
        g_conns[i].paused = true;
        conn_epoll_update(&g_conns[i]);
    }
}

static void resume_all_conns(void) {
    if (!g_conns_paused) return;
    g_conns_paused = false;
    for (int i = 0; i < MAX_CONNS; i++) {
        if (g_conns[i].fd < 0 || !g_conns[i].paused) continue;
        g_conns[i].paused = false;
        conn_epoll_update(&g_conns[i]);
    }
}

// ── frame enqueue ─────────────────────────────────────────────────────────────
static bool enqueue_frame(uint8_t type, uint8_t conn_id,
                          const uint8_t *payload, size_t plen) {
    return pik_link_enqueue(&g_link, type, conn_id, payload, plen);
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
    if (send_close && !enqueue_frame(PIK_TCPBRIDGE_FRAME_CLOSE, (uint8_t)id, NULL, 0))
        LOG("conn %d: unable to queue CLOSE", id);
}

static void close_all_conns(bool send_close) {
    for (int i = 0; i < MAX_CONNS; i++)
        if (g_conns[i].fd >= 0) conn_close(i, send_close);
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

// ── frame handling ────────────────────────────────────────────────────────────
static bool bridge_on_frame(void *ctx, uint8_t type, uint8_t id,
                            const uint8_t *payload, size_t plen) {
    (void)ctx;

    switch (type) {
    case PIK_TCPBRIDGE_FRAME_OPEN:
        if (g_is_listener) {
            LOG("link failure: received OPEN while in listener mode");
            return false;
        }
        if (id >= MAX_CONNS) {
            LOG("link failure: OPEN with invalid connection id");
            return false;
        }
        if (g_conns[id].fd >= 0) {
            LOG("link failure: duplicate OPEN for active connection");
            return false;
        }
        {
            int fd = tcp_connect_to_target();
            if (fd < 0) {
                LOG("conn %d: connect to %s:%d failed: %s",
                    id, g_fwd_host, g_fwd_port, strerror(errno));
                return enqueue_frame(PIK_TCPBRIDGE_FRAME_CLOSE, id, NULL, 0);
            }
            g_conns[id].fd = fd;
            g_conns[id].epev = 0;
            g_conns[id].paused = g_conns_paused;
            g_conns[id].flow_paused = false;
            g_conns[id].pause_sent = false;
            conn_epoll_update(&g_conns[id]);
        }
        return true;

    case PIK_TCPBRIDGE_FRAME_DATA:
        if (id >= MAX_CONNS) {
            LOG("link failure: DATA with invalid connection id");
            return false;
        }
        if (g_conns[id].fd < 0) {
            LOG("conn %u: DATA for closed connection", id);
            return enqueue_frame(PIK_TCPBRIDGE_FRAME_CLOSE, id, NULL, 0);
        }
        if (!plen) return true;
        conn_drain(&g_conns[id]);
        if (conn_space(&g_conns[id]) < plen) {
            LOG("link failure: conn output buffer overflow despite PAUSE");
            return false;
        }
        conn_push(&g_conns[id], payload, plen);
        conn_drain(&g_conns[id]);
        {
            conn_t *c = &g_conns[id];
            uint32_t avail = conn_avail(c);
            if (!c->pause_sent && avail > CONN_HIGH_WATER) {
                if (!enqueue_frame(PIK_TCPBRIDGE_FRAME_PAUSE, id, NULL, 0))
                    return false;
                c->pause_sent = true;
            } else if (c->pause_sent && avail < CONN_LOW_WATER) {
                if (!enqueue_frame(PIK_TCPBRIDGE_FRAME_RESUME, id, NULL, 0))
                    return false;
                c->pause_sent = false;
            }
        }
        conn_epoll_update(&g_conns[id]);
        return true;

    case PIK_TCPBRIDGE_FRAME_CLOSE:
        if (id >= MAX_CONNS) {
            LOG("link failure: CLOSE with invalid connection id");
            return false;
        }
        if (g_conns[id].fd < 0) {
            LOG("conn %u: duplicate CLOSE for closed connection", id);
            return true;
        }
        conn_close(id, false);
        return true;

    case PIK_TCPBRIDGE_FRAME_PAUSE:
        if (id >= MAX_CONNS) {
            LOG("link failure: PAUSE with invalid connection id");
            return false;
        }
        if (g_conns[id].fd < 0) {
            LOG("conn %u: PAUSE for closed connection", id);
            return true;
        }
        g_conns[id].flow_paused = true;
        conn_epoll_update(&g_conns[id]);
        return true;

    case PIK_TCPBRIDGE_FRAME_RESUME:
        if (id >= MAX_CONNS) {
            LOG("link failure: RESUME with invalid connection id");
            return false;
        }
        if (g_conns[id].fd < 0) {
            LOG("conn %u: RESUME for closed connection", id);
            return true;
        }
        g_conns[id].flow_paused = false;
        conn_epoll_update(&g_conns[id]);
        return true;

    default:
        LOG("link failure: unknown frame type 0x%02x", type);
        return false;
    }
}

// ── link open/close ───────────────────────────────────────────────────────────
static void bridge_on_down(void *ctx) {
    (void)ctx;
    status_notify(false);
    LOG("link down");
    close_all_conns(false);
    g_conns_paused = false;
    g_reconnect_at = pik_now_ms() + pik_backoff_next(&g_backoff_ms, RECONNECT_MAX);
}

static void bridge_try_open(int64_t now) {
    if (g_link.fd >= 0) return;
    /* Silent retry while the device is absent (unplugged peer); the link
     * module logs open failures, which would flood the log at this cadence. */
    if (access(g_dev, R_OK | W_OK) != 0 ||
        !pik_link_open(&g_link, g_dev, now)) {
        g_reconnect_at = now + pik_backoff_next(&g_backoff_ms, RECONNECT_MAX);
        return;
    }
    g_backoff_ms = RECONNECT_MIN;
    status_notify(true);
    LOG("link up");
}

// ── listener / TCP input ──────────────────────────────────────────────────────
static bool bind_is_wildcard(void) {
    return strcmp(g_fwd_host, "") == 0 ||
           strcmp(g_fwd_host, "0.0.0.0") == 0 ||
           strcmp(g_fwd_host, "::") == 0;
}

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
    if (g_link.fd < 0) { LOG("link not up, rejecting"); close(fd); return; }

    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    g_conns[id].fd = fd;
    g_conns[id].epev = 0;
    g_conns[id].paused = g_conns_paused;
    g_conns[id].flow_paused = false;
    g_conns[id].pause_sent = false;
    conn_epoll_update(&g_conns[id]);
    if (!enqueue_frame(PIK_TCPBRIDGE_FRAME_OPEN, (uint8_t)id, NULL, 0)) {
        LOG("conn %d: unable to queue OPEN", id);
        conn_close(id, false);
        return;
    }
}

static void conn_on_readable(int id) {
    conn_t *c = &g_conns[id];
    if (pik_link_tx_avail(&g_link) >= LINK_TX_HIGH ||
        !pik_link_can_queue(&g_link, PIK_TCPBRIDGE_MAX_PAYLOAD)) {
        pause_all_conns();
        return;
    }

    static uint8_t buf[PIK_TCPBRIDGE_MAX_PAYLOAD];
    ssize_t n = read(c->fd, buf, sizeof(buf));
    if (n <= 0) {
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) return;
        conn_close(id, true);
        return;
    }
    if (!enqueue_frame(PIK_TCPBRIDGE_FRAME_DATA, (uint8_t)id, buf, (size_t)n))
        return;
    if (pik_link_tx_avail(&g_link) > LINK_TX_HIGH)
        pause_all_conns();
}

// ── main event loop ───────────────────────────────────────────────────────────
#define MAX_EVENTS 32

static void run(void) {
    g_epfd = epoll_create1(EPOLL_CLOEXEC);
    if (g_epfd < 0) DIE("epoll_create1: %s", strerror(errno));

    for (int i = 0; i < MAX_CONNS; i++) g_conns[i].fd = -1;

    pik_link_cfg_t lcfg = {
        .name        = "tcp",
        .nak_type    = PIK_TCPBRIDGE_FRAME_NAK,
        .has_aux     = true,
        .first_type  = 0,   /* first frame must carry seq 0 */
        .heal_from_zero = false, /* bridges restart independently: restart
                                  * with a fresh session rather than replay
                                  * tunneled TCP a dead peer already relayed */
        .max_payload = PIK_TCPBRIDGE_MAX_PAYLOAD,
        .txbuf       = s_link_tx,       .tx_cap     = LINK_TX_CAP,
        .rxbuf       = s_link_rx,       .rx_cap     = sizeof(s_link_rx),
        .hist        = s_link_hist,     .hist_cap   = HIST_CAP,
        .hist_ent    = s_link_hist_ent, .hist_slots = HIST_SLOTS,
        .tx_rate_bps = LINK_TX_RATE_BPS,
        .tx_burst    = LINK_TX_BURST,
        .tx_write_budget = LINK_WRITE_BUDGET,
        .on_frame    = bridge_on_frame,
        .on_down     = bridge_on_down,
        .ctx         = NULL,
    };
    pik_link_init(&g_link, &lcfg, g_epfd);

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
        LOG("listening on %s:%d", g_fwd_host[0] ? g_fwd_host : "0.0.0.0",
            g_fwd_port);
        if (bind_is_wildcard())
            LOG("warning: unauthenticated TCP tunnel is exposed on all interfaces");
    }

    int64_t now = pik_now_ms();
    g_backoff_ms = RECONNECT_MIN;
    bridge_try_open(now);

    for (;;) {
        now = pik_now_ms();
        if (g_link.fd >= 0)
            pik_link_tick(&g_link, now);

        int64_t dl = (g_link.fd < 0) ? g_reconnect_at
                                     : pik_link_deadline(&g_link);

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

            if (pik_link_owns_event(&g_link, ptr)) {
                pik_link_dispatch(&g_link, ev, now);
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
                        if (enqueue_frame(PIK_TCPBRIDGE_FRAME_RESUME, (uint8_t)id, NULL, 0))
                            c->pause_sent = false;
                    }
                    conn_epoll_update(c);
                }
                if (c->fd >= 0 && (ev & EPOLLHUP)) conn_close(id, true);
            }
        }

        now = pik_now_ms();
        if (g_link.fd < 0) {
            if (now >= g_reconnect_at) bridge_try_open(now);
        }

        if (!g_conns_paused && pik_link_tx_avail(&g_link) > LINK_TX_HIGH)
            pause_all_conns();
        else if (g_conns_paused && pik_link_tx_avail(&g_link) < LINK_TX_LOW)
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
    signal(SIGPIPE, SIG_IGN);

    const char *status_fd = getenv(TCP_STATUS_FD_ENV);
    if (status_fd && *status_fd) {
        char *end = NULL;
        long fd = strtol(status_fd, &end, 10);
        if (end && *end == '\0' && fd >= 0 && fd <= INT_MAX)
            g_status_fd = (int)fd;
    }

    strncpy(g_dev, argv[1], sizeof(g_dev) - 1);

    const char *mode = argv[2];
    if (strcmp(mode, "listen") == 0)
        g_is_listener = true;
    else if (strcmp(mode, "forward") == 0)
        g_is_listener = false;
    else
        usage(argv[0]);
    pik_log_set_timestamps(g_is_listener);

    const char *hostport = argv[3];
    const char *colon = strrchr(hostport, ':');
    if (!colon) usage(argv[0]);
    size_t hlen = (size_t)(colon - hostport);
    if (hlen >= sizeof(g_fwd_host)) usage(argv[0]);
    memcpy(g_fwd_host, hostport, hlen);
    g_fwd_host[hlen] = '\0';
    if (!pik_parse_port(colon + 1, &g_fwd_port)) usage(argv[0]);

    LOG("tcpbridge %s %s %s:%d",
        g_dev, g_is_listener ? "listen" : "forward",
        g_fwd_host, g_fwd_port);
    run();
    return 0;
}
