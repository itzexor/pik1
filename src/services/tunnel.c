// TCP tunnel service: dynamic streams on channel 15 of the shared link

#include "tunnel.h"
#include "pik_proto.h"
#include "session.h"
#include "fd.h"
#include "logging.h"
#include "util.h"
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#define MAX_CONNS         16
#define CONN_RING_CAP     (1u << 18)
#define CONN_RING_MASK    (CONN_RING_CAP - 1u)
#define CONN_HIGH_WATER   (64u  * 1024u)
#define CONN_LOW_WATER    (16u  * 1024u)

#define TUN_HIGH_WATER    (PIK_SESSION_TUN_QUEUE_CAP / 4u)
#define TUN_LOW_WATER     (PIK_SESSION_TUN_QUEUE_CAP / 16u)

#define LISTEN_RETRY_MIN  1000
#define LISTEN_RETRY_MAX  30000

typedef struct {
    int      fd;
    uint32_t epev;
    uint8_t  gen;          /* current nonzero incarnation of this slot */
    uint8_t  txbuf[CONN_RING_CAP];
    uint32_t tx_head, tx_tail;
    bool     paused;       /* local shared-link backpressure */
    bool     flow_paused;  /* remote sent PAUSE */
    bool     pause_sent;   /* we sent PAUSE */
} conn_t;

static conn_t        g_conns[MAX_CONNS];
static tunnel_mode_t g_mode = TUNNEL_MODE_NONE;
static bool          g_started;
static bool          g_conns_paused;   /* class high-water pause of all conns */
static char          g_host[64];
static int           g_port;
static int           g_listen_fd = -1;
static int           g_epfd      = -1;
static int64_t       g_listen_retry_at;
static int           g_listen_backoff_ms;

static int g_listen_tag;

#define LOG(...) pik_log("tun", __VA_ARGS__)

static uint32_t conn_avail(const conn_t *c) { return c->tx_tail - c->tx_head; }
static uint32_t conn_space(const conn_t *c) { return CONN_RING_CAP - conn_avail(c); }

static void conn_push(conn_t *c, const uint8_t *src, size_t len) {
    for (size_t i = 0; i < len; i++)
        c->txbuf[c->tx_tail++ & CONN_RING_MASK] = src[i];
}

static bool conn_drain(conn_t *c) {
    while (conn_avail(c) && c->fd >= 0) {
        uint32_t off    = c->tx_head & CONN_RING_MASK;
        uint32_t contig = CONN_RING_CAP - off;
        uint32_t avail  = conn_avail(c);
        size_t   n      = avail < contig ? avail : contig;
        ssize_t  w      = write(c->fd, c->txbuf + off, n);
        if (w < 0 && errno == EINTR)
            continue;
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return true;
        if (w <= 0) {
            if (w < 0)
                LOG("conn %d TCP write: %s",
                    (int)(c - g_conns), strerror(errno));
            return false;
        }
        c->tx_head += (uint32_t)w;
    }
    return true;
}

static void conn_epoll_update(conn_t *c) {
    if (c->fd < 0) return;
    uint32_t want = ((!c->paused && !c->flow_paused) ? EPOLLIN : 0u)
                  | (conn_avail(c) ? EPOLLOUT : 0u);
    if (want == c->epev) return;
    c->epev = want;
    if (want && !pik_epoll_set(g_epfd, c->fd, want, c)) {
        LOG("conn %d epoll update: %s",
            (int)(c - g_conns), strerror(errno));
        pik_session_fail();
    } else if (!want) {
        pik_epoll_del(g_epfd, c->fd);
    }
}

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

static bool enqueue_frame(uint8_t type, uint8_t conn_id, uint8_t gen,
                          const uint8_t *data, size_t dlen) {
    static uint8_t payload[PIK_TUN_MAX_PAYLOAD];
    payload[0] = conn_id;
    payload[1] = gen;
    if (dlen) memcpy(payload + PIK_TUN_PREFIX_LEN, data, dlen);
    return pik_session_enqueue(PIK_SESSION_CLASS_TUNNEL, type, PIK_CH_TUNNEL,
                               payload, PIK_TUN_PREFIX_LEN + dlen);
}

static bool is_stale_closed_slot(const conn_t *c, uint8_t gen) {
    return c->fd < 0 && c->gen != 0 && gen != c->gen;
}

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
    if (send_close &&
        !enqueue_frame(PIK_FRAME_TUN_CLOSE, (uint8_t)id, c->gen, NULL, 0))
        LOG("conn %d: unable to queue CLOSE", id);
}

static void close_all_conns(bool send_close) {
    for (int i = 0; i < MAX_CONNS; i++)
        if (g_conns[i].fd >= 0) conn_close(i, send_close);
}

static int tcp_connect_to_target(void) {
    struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM };
    struct addrinfo *res = NULL;
    char port[8];
    snprintf(port, sizeof(port), "%d", g_port);
    if (getaddrinfo(g_host, port, &hints, &res) != 0 || !res) return -1;

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

bool tunnel_on_frame(uint8_t type, const uint8_t *payload, size_t plen) {
    if (g_mode == TUNNEL_MODE_NONE) {
        LOG("link failure: tunnel frame but no tunnel configured");
        return false;
    }
    if (plen < PIK_TUN_PREFIX_LEN || plen > PIK_TUN_MAX_PAYLOAD) {
        LOG("link failure: bad tunnel frame len=%zu", plen);
        return false;
    }
    uint8_t id = payload[0];
    uint8_t gen = payload[1];
    const uint8_t *data = payload + PIK_TUN_PREFIX_LEN;
    size_t dlen = plen - PIK_TUN_PREFIX_LEN;

    if (gen == 0) {
        LOG("link failure: zero tunnel generation");
        return false;
    }
    if (id >= MAX_CONNS) {
        LOG("link failure: invalid connection id %u type=0x%02x", id, type);
        return false;
    }
    if (type != PIK_FRAME_TUN_DATA && dlen != 0) {
        LOG("link failure: tunnel control frame type=0x%02x has %zu data bytes",
            type, dlen);
        return false;
    }
    conn_t *c = &g_conns[id];

    switch (type) {
    case PIK_FRAME_TUN_OPEN:
        if (g_mode == TUNNEL_MODE_LISTEN) {
            LOG("link failure: received OPEN while in listener mode");
            return false;
        }
        if (c->fd >= 0) {
            LOG("link failure: duplicate OPEN for active connection");
            return false;
        }
        {
            int fd = tcp_connect_to_target();
            if (fd < 0) {
                LOG("conn %d: connect to %s:%d failed: %s",
                    id, g_host, g_port, strerror(errno));
                return enqueue_frame(PIK_FRAME_TUN_CLOSE, id, gen, NULL, 0);
            }
            c->fd = fd;
            c->epev = 0;
            c->gen = gen;
            c->paused = g_conns_paused;
            c->flow_paused = false;
            c->pause_sent = false;
            conn_epoll_update(c);
        }
        return true;

    case PIK_FRAME_TUN_DATA:
        if (is_stale_closed_slot(c, gen)) return true;
        if (c->fd < 0) {
            LOG("conn %u: DATA for closed connection", id);
            return enqueue_frame(PIK_FRAME_TUN_CLOSE, id, gen, NULL, 0);
        }
        if (gen != c->gen) return true;  /* stale incarnation, conn is gone */
        if (!dlen) return true;
        if (!conn_drain(c)) {
            conn_close(id, true);
            return pik_session_up();
        }
        if (conn_space(c) < dlen) {
            LOG("link failure: conn output buffer overflow despite PAUSE");
            return false;
        }
        conn_push(c, data, dlen);
        if (!conn_drain(c)) {
            conn_close(id, true);
            return pik_session_up();
        }
        {
            uint32_t avail = conn_avail(c);
            if (!c->pause_sent && avail > CONN_HIGH_WATER) {
                if (!enqueue_frame(PIK_FRAME_TUN_PAUSE, id, c->gen, NULL, 0))
                    return false;
                c->pause_sent = true;
            } else if (c->pause_sent && avail < CONN_LOW_WATER) {
                if (!enqueue_frame(PIK_FRAME_TUN_RESUME, id, c->gen, NULL, 0))
                    return false;
                c->pause_sent = false;
            }
        }
        conn_epoll_update(c);
        return true;

    case PIK_FRAME_TUN_CLOSE:
        if (is_stale_closed_slot(c, gen)) return true;
        if (c->fd < 0) {
            LOG("conn %u: CLOSE for closed connection", id);
            return true;
        }
        if (gen != c->gen) return true;
        conn_close(id, false);
        return true;

    case PIK_FRAME_TUN_PAUSE:
        if (is_stale_closed_slot(c, gen)) return true;
        if (c->fd < 0) {
            LOG("conn %u: PAUSE for closed connection", id);
            return true;
        }
        if (gen != c->gen) return true;
        c->flow_paused = true;
        conn_epoll_update(c);
        return true;

    case PIK_FRAME_TUN_RESUME:
        if (is_stale_closed_slot(c, gen)) return true;
        if (c->fd < 0) {
            LOG("conn %u: RESUME for closed connection", id);
            return true;
        }
        if (gen != c->gen) return true;
        c->flow_paused = false;
        conn_epoll_update(c);
        return true;

    default:
        LOG("link failure: unknown tunnel frame type 0x%02x", type);
        return false;
    }
}

static void listener_stop(void) {
    if (g_listen_fd < 0) return;
    pik_epoll_del(g_epfd, g_listen_fd);
    close(g_listen_fd);
    g_listen_fd = -1;
}

static bool listener_start(void) {
    if (g_mode != TUNNEL_MODE_LISTEN || g_listen_fd >= 0) return true;

    int one = 1;
    bool wildcard = strcmp(g_host, "0.0.0.0") == 0;
    struct sockaddr_in sa = {
        .sin_family = AF_INET,
        .sin_port = htons((uint16_t)g_port),
    };
    if (wildcard) {
        sa.sin_addr.s_addr = INADDR_ANY;
    } else if (inet_pton(AF_INET, g_host, &sa.sin_addr) != 1) {
        LOG("bad bind address: %s", g_host);
        return false;
    }

    int lfd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (lfd < 0) {
        LOG("listen socket: %s", strerror(errno));
        return false;
    }
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (bind(lfd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        LOG("bind %s:%d: %s", g_host, g_port, strerror(errno));
        close(lfd);
        return false;
    }
    if (listen(lfd, 16) < 0) {
        LOG("listen: %s", strerror(errno));
        close(lfd);
        return false;
    }
    g_listen_fd = lfd;
    if (!pik_epoll_set(g_epfd, lfd, EPOLLIN, &g_listen_tag)) {
        LOG("epoll add tunnel listener: %s", strerror(errno));
        close(lfd);
        g_listen_fd = -1;
        return false;
    }
    LOG("listening on %s:%d", g_host, g_port);
    if (wildcard)
        LOG("warning: unauthenticated TCP tunnel is exposed on all interfaces");
    return true;
}

static void listener_start_or_retry(int64_t now) {
    if (listener_start()) {
        g_listen_retry_at = 0;
        g_listen_backoff_ms = LISTEN_RETRY_MIN;
    } else {
        g_listen_retry_at =
            now + pik_backoff_next(&g_listen_backoff_ms, LISTEN_RETRY_MAX);
    }
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
    if (!pik_session_up() || !g_started) {
        LOG("link not up, rejecting");
        close(fd);
        return;
    }

    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    conn_t *c = &g_conns[id];
    c->fd = fd;
    c->epev = 0;
    c->gen++;
    if (c->gen == 0) c->gen++;
    c->paused = g_conns_paused;
    c->flow_paused = false;
    c->pause_sent = false;
    conn_epoll_update(c);
    if (!enqueue_frame(PIK_FRAME_TUN_OPEN, (uint8_t)id, c->gen, NULL, 0)) {
        LOG("conn %d: unable to queue OPEN", id);
        conn_close(id, false);
        return;
    }
}

static void conn_on_readable(int id) {
    conn_t *c = &g_conns[id];
    if (pik_session_backlog(PIK_SESSION_CLASS_TUNNEL) >= TUN_HIGH_WATER ||
        !pik_session_can_queue(PIK_SESSION_CLASS_TUNNEL, PIK_TUN_MAX_PAYLOAD)) {
        pause_all_conns();
        return;
    }

    static uint8_t buf[PIK_TUN_MAX_DATA];
    ssize_t n = read(c->fd, buf, sizeof(buf));
    if (n <= 0) {
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) return;
        conn_close(id, true);
        return;
    }
    if (!enqueue_frame(PIK_FRAME_TUN_DATA, (uint8_t)id, c->gen, buf, (size_t)n))
        return;
    if (pik_session_backlog(PIK_SESSION_CLASS_TUNNEL) > TUN_HIGH_WATER)
        pause_all_conns();
}

void tunnel_on_link_down(void) {
    if (g_mode == TUNNEL_MODE_NONE || !g_started) return;
    LOG("link down");
    g_started = false;
    listener_stop();
    g_listen_retry_at = 0;
    close_all_conns(false);
    g_conns_paused = false;
}

void tunnel_init(int epfd, tunnel_mode_t mode, const char *host, int port) {
    g_epfd = epfd;
    g_mode = mode;
    g_port = port;
    g_started = false;
    g_conns_paused = false;
    g_listen_retry_at = 0;
    g_listen_backoff_ms = LISTEN_RETRY_MIN;
    if (host)
        snprintf(g_host, sizeof(g_host), "%s", host);
    for (int i = 0; i < MAX_CONNS; i++) {
        g_conns[i].fd = -1;
        g_conns[i].gen = 0;
    }
}

void tunnel_start(int64_t now) {
    if (g_mode == TUNNEL_MODE_NONE) return;
    g_started = true;
    g_conns_paused = false;
    if (g_mode == TUNNEL_MODE_LISTEN)
        listener_start_or_retry(now);
}

bool tunnel_owns_event(const void *ptr) {
    if (ptr == &g_listen_tag) return true;
    for (int i = 0; i < MAX_CONNS; i++)
        if (ptr == &g_conns[i]) return true;
    return false;
}

bool tunnel_dispatch(void *ptr, uint32_t events) {
    if (ptr == &g_listen_tag) {
        if (events & (EPOLLERR | EPOLLHUP)) {
            LOG("listener event failure: events=0x%x", events);
            listener_stop();
            g_listen_retry_at =
                pik_now_ms() +
                pik_backoff_next(&g_listen_backoff_ms, LISTEN_RETRY_MAX);
            return pik_session_up();
        }
        listener_accept();
        return pik_session_up();
    }
    conn_t *c = (conn_t *)ptr;
    int id = (int)(c - g_conns);
    if (events & EPOLLERR) { conn_close(id, true); return pik_session_up(); }
    if (events & EPOLLIN) conn_on_readable(id);
    if (c->fd >= 0 && (events & EPOLLOUT)) {
        if (!conn_drain(c)) {
            conn_close(id, true);
            return pik_session_up();
        }
        if (c->pause_sent && conn_avail(c) < CONN_LOW_WATER) {
            if (enqueue_frame(PIK_FRAME_TUN_RESUME, (uint8_t)id, c->gen, NULL, 0))
                c->pause_sent = false;
        }
        conn_epoll_update(c);
    }
    if (c->fd >= 0 && (events & EPOLLHUP)) conn_close(id, true);
    return pik_session_up();
}

bool tunnel_tick(int64_t now) {
    if (g_mode == TUNNEL_MODE_NONE || !g_started) return true;
    if (!pik_session_up()) return false;

    if (g_listen_retry_at && now >= g_listen_retry_at)
        listener_start_or_retry(now);

    if (!g_conns_paused &&
        pik_session_backlog(PIK_SESSION_CLASS_TUNNEL) > TUN_HIGH_WATER)
        pause_all_conns();
    else if (g_conns_paused &&
             pik_session_backlog(PIK_SESSION_CLASS_TUNNEL) < TUN_LOW_WATER &&
             pik_session_can_queue(PIK_SESSION_CLASS_TUNNEL, PIK_TUN_MAX_PAYLOAD))
        resume_all_conns();
    return pik_session_up();
}

int64_t tunnel_deadline(void) {
    if (!g_started || !g_listen_retry_at) return INT64_MAX;
    return g_listen_retry_at;
}

bool tunnel_active(void) {
    if (g_mode == TUNNEL_MODE_NONE || !g_started) return false;
    if (g_mode == TUNNEL_MODE_LISTEN) return g_listen_fd >= 0;
    return true;
}

void tunnel_cleanup(void) {
    listener_stop();
    close_all_conns(false);
    g_started = false;
    g_conns_paused = false;
    g_listen_retry_at = 0;
}
