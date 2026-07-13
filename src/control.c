#include "control.h"
#include "control_proto.h"
#include "nanocobs/cobs.h"
#include "fd.h"
#include "frame.h"
#include "logging.h"
#include "tty.h"
#include "util.h"
#include "version.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>

#define FRAME_DEC_MAX (PIK_CONTROL_FRAME_HEADER_LEN + PIK_CONTROL_MAX_PAYLOAD + 4u)
#define FRAME_ENC_MAX COBS_ENCODE_MAX(FRAME_DEC_MAX)

#define TX_RING_CAP   8192u
#define TX_RING_MASK  (TX_RING_CAP - 1u)

/* Retransmit history: encoded copies of the most recent sequenced TX frames,
 * kept so a peer NAK can be answered byte-identically (FAILURE_MODEL.md,
 * "Documented Exceptions"). */
#define HIST_CAP        4096u
#define HIST_MASK       (HIST_CAP - 1u)
#define HIST_SLOTS      64u         /* power of two, bounds frame count */

#define NAK_RETRY_MS    100         /* re-send NAK while a gap persists */
#define GAP_BUDGET_MS   500         /* unhealed gap fails the link */
#define STALE_GRACE_MS  2000        /* discard stale frames after link open */

#define HELLO_RETRY_MS 2000
#define PING_IDLE_MS  3000
#define LINK_DEAD_MS  10000

typedef struct {
    uint32_t off;   /* free-running byte offset into hist ring */
    uint16_t len;
} hist_ent_t;

typedef struct {
    int fd;
    bool ready;
    bool failed;
    uint32_t epev;

    uint8_t txbuf[TX_RING_CAP];
    uint32_t tx_head, tx_tail;

    uint8_t rxbuf[FRAME_ENC_MAX + 4];
    size_t rxbuf_len;

    uint32_t tx_session;
    uint32_t rx_session;
    uint16_t tx_seq;
    uint16_t rx_seq;

    /* retransmit history of sent frames (encoded bytes + per-seq index) */
    uint8_t hist[HIST_CAP];
    hist_ent_t hist_ent[HIST_SLOTS];
    uint16_t hist_first;            /* seq of oldest frame still held */
    uint16_t hist_count;
    uint32_t hist_head, hist_tail;
    int64_t last_resend_ms;

    /* receiver gap recovery */
    int64_t gap_since_ms;           /* 0 = stream in sync */
    int64_t last_nak_ms;
    uint32_t gap_discards;
    uint32_t dup_discards;

    /* bring-up grace for stale frames from the peer's previous session */
    int64_t opened_ms;
    uint32_t stale_discards;

    int64_t last_tx_ms;
    int64_t last_rx_ms;

    pik_control_role_t role;
    pik_control_command_fn on_command;
    void *ctx;

    uint32_t next_request_id;
    bool ack_pending;
    uint32_t ack_request_id;
    pik_control_ack_status_t ack_status;
    uint8_t ack_payload[PIK_CONTROL_MAX_PAYLOAD - 5u];
    size_t ack_payload_len;

    bool peer_link_known;
    uint32_t peer_link_flags;

    uint8_t local_channels[PIK_CONTROL_MAX_PAYLOAD - 2u];
    size_t local_channel_count;
    pik_control_tcp_role_t local_tcp_role;
    bool config_sent;
} control_t;

static control_t g_ctrl;
static int g_epfd = -1;
static int g_ctrl_tag;
static int64_t g_now_ms;    /* updated on entry to start/dispatch/tick */

/* Cumulative I/O counters (process lifetime, like tcpbridge's): dumped on
 * every link failure so a dead link leaves evidence of what it was doing. */
static uint64_t g_rx_frames;
static uint64_t g_tx_frames;
static uint64_t g_rx_reads;
static uint64_t g_rx_bytes;
static uint64_t g_tx_writes;
static uint64_t g_tx_bytes;

#define LOG(...) pik_log("ctrl", __VA_ARGS__)

static void close_link(void);

static uint32_t avail(void) { return g_ctrl.tx_tail - g_ctrl.tx_head; }
static uint32_t space(void) { return TX_RING_CAP - avail(); }

const char *pik_control_action_name(pik_control_action_t action) {
    switch (action) {
    case PIK_CONTROL_ACTION_RESTART_PEER: return "restart-peer";
    case PIK_CONTROL_ACTION_REBOOT_PEER: return "reboot-peer";
    case PIK_CONTROL_ACTION_POWEROFF_PEER: return "poweroff-peer";
    case PIK_CONTROL_ACTION_STATUS: return "status";
    default: return "unknown";
    }
}

const char *pik_control_ack_status_name(pik_control_ack_status_t status) {
    switch (status) {
    case PIK_CONTROL_ACK_OK: return "ok";
    case PIK_CONTROL_ACK_UNKNOWN_ACTION: return "unknown-action";
    case PIK_CONTROL_ACK_INTERNAL_ERROR: return "internal-error";
    default: return "unknown-status";
    }
}

static bool action_valid(pik_control_action_t action) {
    switch (action) {
    case PIK_CONTROL_ACTION_RESTART_PEER:
    case PIK_CONTROL_ACTION_REBOOT_PEER:
    case PIK_CONTROL_ACTION_POWEROFF_PEER:
    case PIK_CONTROL_ACTION_STATUS:
        return true;
    default:
        return false;
    }
}

static bool ack_status_valid(pik_control_ack_status_t status) {
    switch (status) {
    case PIK_CONTROL_ACK_OK:
    case PIK_CONTROL_ACK_UNKNOWN_ACTION:
    case PIK_CONTROL_ACK_INTERNAL_ERROR:
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
    g_ctrl.ack_payload_len = 0;
}

static void update_epoll(void) {
    if (g_ctrl.fd < 0) return;
    uint32_t want = EPOLLIN | (avail() ? EPOLLOUT : 0u);
    if (want == g_ctrl.epev) return;
    g_ctrl.epev = want;
    pik_epoll_set(g_epfd, g_ctrl.fd, want, &g_ctrl_tag);
}

static void hist_store(uint16_t seq, const uint8_t *enc, size_t enc_len) {
    while (g_ctrl.hist_count &&
           (g_ctrl.hist_count == HIST_SLOTS ||
            HIST_CAP - (g_ctrl.hist_tail - g_ctrl.hist_head) < (uint32_t)enc_len)) {
        g_ctrl.hist_head += g_ctrl.hist_ent[g_ctrl.hist_first % HIST_SLOTS].len;
        g_ctrl.hist_first++;
        g_ctrl.hist_count--;
    }
    if (!g_ctrl.hist_count) {
        g_ctrl.hist_first = seq;
        g_ctrl.hist_head = g_ctrl.hist_tail;
    }
    hist_ent_t *e = &g_ctrl.hist_ent[seq % HIST_SLOTS];
    e->off = g_ctrl.hist_tail;
    e->len = (uint16_t)enc_len;
    for (size_t i = 0; i < enc_len; i++)
        g_ctrl.hist[(g_ctrl.hist_tail + i) & HIST_MASK] = enc[i];
    g_ctrl.hist_tail += (uint32_t)enc_len;
    g_ctrl.hist_count++;
}

/* sequenced=false is for link-control frames (NAK): they consume no sequence
 * number and are never retransmitted, so a peer can process them mid-gap. */
static bool enqueue_frame_opt(uint8_t type, const uint8_t *payload, size_t plen,
                              bool sequenced) {
    if (plen > PIK_CONTROL_MAX_PAYLOAD) {
        LOG("oversized frame type=0x%02x plen=%zu", type, plen);
        close_link();
        return false;
    }

    static uint8_t dec[FRAME_DEC_MAX];
    static uint8_t enc[FRAME_ENC_MAX + 1];
    uint8_t header[PIK_CONTROL_FRAME_HEADER_LEN];
    size_t enc_len = 0;

    header[0] = type;
    pik_put_u32le(header + 1, g_ctrl.tx_session);
    header[5] = (uint8_t)g_ctrl.tx_seq;
    header[6] = (uint8_t)(g_ctrl.tx_seq >> 8);

    if (pik_frame_encode(header, sizeof(header), payload, plen,
                         dec, sizeof(dec), enc, sizeof(enc), &enc_len) != PIK_FRAME_OK) {
        LOG("encode failed type=0x%02x", type);
        close_link();
        return false;
    }
    if (!tx_push(enc, enc_len)) {
        LOG("TX ring full, closing before dropping type=0x%02x", type);
        close_link();
        return false;
    }
    if (sequenced) {
        hist_store(g_ctrl.tx_seq, enc, enc_len);
        g_ctrl.tx_seq++;
    }
    g_tx_frames++;
    update_epoll();
    return true;
}

static bool enqueue_frame(uint8_t type, const uint8_t *payload, size_t plen) {
    return enqueue_frame_opt(type, payload, plen, true);
}

static void send_nak(int64_t now) {
    uint8_t p[2] = { (uint8_t)g_ctrl.rx_seq, (uint8_t)(g_ctrl.rx_seq >> 8) };
    enqueue_frame_opt(PIK_CONTROL_FRAME_NAK, p, sizeof(p), false);
    g_ctrl.last_nak_ms = now;
}

static bool handle_nak(const uint8_t *p) {
    uint16_t expected = (uint16_t)p[0] | (uint16_t)p[1] << 8;
    uint16_t outstanding = (uint16_t)(g_ctrl.tx_seq - expected);
    if (outstanding == 0) return true; /* peer caught up while the NAK was in flight */
    if (outstanding > g_ctrl.hist_count) {
        LOG("link failure: NAK beyond retransmit window expected=%u tx_seq=%u window=%u",
            expected, g_ctrl.tx_seq, g_ctrl.hist_count);
        close_link();
        return false;
    }
    if (g_ctrl.last_resend_ms && g_now_ms - g_ctrl.last_resend_ms < NAK_RETRY_MS / 2)
        return true; /* duplicate NAK burst; resend already queued */

    LOG("retransmit: seq=%u..%u frames=%u (peer NAK)",
        expected, (uint16_t)(g_ctrl.tx_seq - 1u), outstanding);
    for (uint16_t s = expected; s != g_ctrl.tx_seq; s++) {
        const hist_ent_t *e = &g_ctrl.hist_ent[s % HIST_SLOTS];
        if (space() < e->len) {
            LOG("TX ring full during retransmit");
            close_link();
            return false;
        }
        for (uint16_t i = 0; i < e->len; i++)
            g_ctrl.txbuf[g_ctrl.tx_tail++ & TX_RING_MASK] =
                g_ctrl.hist[(e->off + i) & HIST_MASK];
    }
    g_ctrl.last_resend_ms = g_now_ms;
    update_epoll();
    return true;
}

static bool send_hello(void) {
    uint8_t p[PIK_CONTROL_HELLO_LEN];
    memset(p, 0, sizeof(p));
    p[0] = PIK_CONTROL_HELLO_MAGIC_0;
    p[1] = PIK_CONTROL_HELLO_MAGIC_1;
    p[2] = PIK_CONTROL_HELLO_MAGIC_2;
    p[3] = PIK_CONTROL_HELLO_MAGIC_3;
    pik_put_u32le(p + 4, PIK1_PROTOCOL_VERSION);
    pik_put_u32le(p + 8, PIK1_FEATURE_FLAGS);
    p[12] = (uint8_t)g_ctrl.role;
    snprintf((char *)p + 13, PIK_CONTROL_RELEASE_LEN, "%s", PIK1_RELEASE_VERSION);
    return enqueue_frame(PIK_CONTROL_FRAME_HELLO, p, sizeof(p));
}

static const char *tcp_role_name(pik_control_tcp_role_t role) {
    switch (role) {
    case PIK_CONTROL_TCP_NONE: return "none";
    case PIK_CONTROL_TCP_LISTEN: return "listen";
    case PIK_CONTROL_TCP_FORWARD: return "forward";
    default: return "unknown";
    }
}

static bool send_config(void) {
    uint8_t p[PIK_CONTROL_MAX_PAYLOAD];
    if (g_ctrl.local_channel_count > sizeof(p) - 2)
        return false;
    p[0] = (uint8_t)g_ctrl.local_tcp_role;
    p[1] = (uint8_t)g_ctrl.local_channel_count;
    if (g_ctrl.local_channel_count)
        memcpy(p + 2, g_ctrl.local_channels, g_ctrl.local_channel_count);
    if (!enqueue_frame(PIK_CONTROL_FRAME_CONFIG, p, g_ctrl.local_channel_count + 2))
        return false;
    g_ctrl.config_sent = true;
    return true;
}

static bool local_channel_present(uint8_t id) {
    for (size_t i = 0; i < g_ctrl.local_channel_count; i++)
        if (g_ctrl.local_channels[i] == id)
            return true;
    return false;
}

static bool handle_config(const uint8_t *p, size_t len) {
    if (len < 2 || p[1] != len - 2) {
        LOG("bad CONFIG");
        close_link();
        return false;
    }

    pik_control_tcp_role_t peer_tcp = (pik_control_tcp_role_t)p[0];
    if (peer_tcp > PIK_CONTROL_TCP_FORWARD) {
        LOG("bad CONFIG tcp role=%u", p[0]);
        close_link();
        return false;
    }

    bool peer_present[UINT8_MAX + 1] = { false };
    for (size_t i = 2; i < len; i++) {
        uint8_t id = p[i];
        if (peer_present[id]) continue;
        peer_present[id] = true;
        if (!local_channel_present(id))
            LOG("warning: channel id %u is not configured on this side", id);
    }

    for (size_t i = 0; i < g_ctrl.local_channel_count; i++) {
        uint8_t id = g_ctrl.local_channels[i];
        if (!peer_present[id])
            LOG("warning: channel id %u is not configured on peer", id);
    }

    if (g_ctrl.local_tcp_role == PIK_CONTROL_TCP_NONE &&
        peer_tcp != PIK_CONTROL_TCP_NONE) {
        LOG("warning: TCP tunnel is configured on peer as %s but not on this side",
            tcp_role_name(peer_tcp));
    } else if (g_ctrl.local_tcp_role != PIK_CONTROL_TCP_NONE &&
               peer_tcp == PIK_CONTROL_TCP_NONE) {
        LOG("warning: TCP tunnel is configured on this side as %s but not on peer",
            tcp_role_name(g_ctrl.local_tcp_role));
    } else if (g_ctrl.local_tcp_role != PIK_CONTROL_TCP_NONE &&
               g_ctrl.local_tcp_role == peer_tcp) {
        LOG("warning: TCP tunnel is configured as %s on both sides",
            tcp_role_name(g_ctrl.local_tcp_role));
    }
    return true;
}

static void close_link(void) {
    g_ctrl.failed = true;
    if (g_ctrl.fd >= 0) {
        LOG("link stats: rx_frames=%llu tx_frames=%llu rx_reads=%llu rx_bytes=%llu "
            "tx_writes=%llu tx_bytes=%llu rx_seq=%u tx_seq=%u",
            (unsigned long long)g_rx_frames, (unsigned long long)g_tx_frames,
            (unsigned long long)g_rx_reads, (unsigned long long)g_rx_bytes,
            (unsigned long long)g_tx_writes, (unsigned long long)g_tx_bytes,
            g_ctrl.rx_seq, g_ctrl.tx_seq);
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
    if (len != PIK_CONTROL_HELLO_LEN ||
        p[0] != PIK_CONTROL_HELLO_MAGIC_0 || p[1] != PIK_CONTROL_HELLO_MAGIC_1 ||
        p[2] != PIK_CONTROL_HELLO_MAGIC_2 || p[3] != PIK_CONTROL_HELLO_MAGIC_3) {
        LOG("bad HELLO");
        close_link();
        return false;
    }

    uint32_t proto = pik_get_u32le(p + 4);
    uint32_t features = pik_get_u32le(p + 8);
    uint8_t role = p[12];
    char release[PIK_CONTROL_RELEASE_LEN + 1];
    memcpy(release, p + 13, PIK_CONTROL_RELEASE_LEN);
    release[PIK_CONTROL_RELEASE_LEN] = '\0';

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
        if (!send_hello())
            return false;
        if (!send_config())
            return false;
    }
    return true;
}

static bool dispatch_frame(const uint8_t *enc, size_t enc_len) {
    static uint8_t dec[FRAME_DEC_MAX];
    pik_frame_t frame;
    g_rx_frames++;

    pik_frame_status_t st = pik_frame_decode(enc, enc_len, FRAME_ENC_MAX + 1,
                                             PIK_CONTROL_FRAME_HEADER_LEN, dec, sizeof(dec), &frame);
    if (st != PIK_FRAME_OK) {
        LOG("frame failure: %s enc_len=%zu first=0x%02x",
            pik_frame_status_text(st), enc_len, enc_len ? enc[0] : 0);
        pik_log_bad_frame_sample("ctrl", enc, enc_len);
        close_link();
        return false;
    }

    uint8_t type = frame.header[0];
    uint32_t session = pik_get_u32le(frame.header + 1);
    uint16_t seq = (uint16_t)frame.header[5] | (uint16_t)frame.header[6] << 8;
    const uint8_t *p = frame.payload;
    size_t len = frame.payload_len;

    if (session == 0) {
        LOG("bad control session=0 type=0x%02x", type);
        close_link();
        return false;
    }

    if (type == PIK_CONTROL_FRAME_NAK) {
        /* Link-control: not sequenced, so it remains processable while the
         * peer's inbound stream has a gap. Ignore unless it is provably from
         * the live peer session. */
        if (g_ctrl.rx_session == 0 || session != g_ctrl.rx_session) return true;
        if (len != 2) {
            LOG("bad NAK len=%zu", len);
            close_link();
            return false;
        }
        return handle_nak(p);
    }

    if (g_ctrl.rx_session == 0) {
        if (type != PIK_CONTROL_FRAME_HELLO) {
            /* Stale in-flight frames from the peer's previous session are
             * expected physics of an unsynchronized restart: discard them
             * for a bounded window instead of failing the fresh link. */
            if (g_now_ms - g_ctrl.opened_ms <= STALE_GRACE_MS) {
                if (!g_ctrl.stale_discards)
                    LOG("discarding stale bring-up frames (session=0x%08x seq=%u type=0x%02x)",
                        session, seq, type);
                g_ctrl.stale_discards++;
                return true;
            }
            LOG("bad first control frame type=0x%02x seq=%u discarded=%u",
                type, seq, g_ctrl.stale_discards);
            close_link();
            return false;
        }
        g_ctrl.rx_session = session;
        g_ctrl.rx_seq = seq + 1;
        if (g_ctrl.stale_discards)
            LOG("bring-up synchronized, discarded %u stale frames", g_ctrl.stale_discards);
    } else if (session != g_ctrl.rx_session) {
        LOG("control session changed old=0x%08x new=0x%08x type=0x%02x",
            g_ctrl.rx_session, session, type);
        close_link();
        return false;
    } else {
        int16_t d = (int16_t)(seq - g_ctrl.rx_seq);
        if (d < 0) {
            /* duplicate from retransmission overlap; already delivered */
            g_ctrl.dup_discards++;
            return true;
        }
        if (d > 0) {
            if (!g_ctrl.gap_since_ms) {
                g_ctrl.gap_since_ms = g_now_ms;
                g_ctrl.gap_discards = 0;
                LOG("control seq gap seq=%u expected=%u type=0x%02x, requesting retransmit",
                    seq, g_ctrl.rx_seq, type);
                send_nak(g_now_ms);
            }
            g_ctrl.gap_discards++;
            return !g_ctrl.failed;
        }
        g_ctrl.rx_seq++;
        if (g_ctrl.gap_since_ms) {
            LOG("retransmit healed: seq=%u latency=%lldms discarded=%u dups=%u",
                seq, (long long)(g_now_ms - g_ctrl.gap_since_ms),
                g_ctrl.gap_discards, g_ctrl.dup_discards);
            g_ctrl.gap_since_ms = 0;
            g_ctrl.gap_discards = 0;
            g_ctrl.dup_discards = 0;
        }
    }

    switch (type) {
    case PIK_CONTROL_FRAME_HELLO:
        return handle_hello(p, len);
    case PIK_CONTROL_FRAME_PING:
        if (len != 0) {
            LOG("bad PING len=%zu", len);
            close_link();
            return false;
        }
        return enqueue_frame(PIK_CONTROL_FRAME_PONG, NULL, 0);
    case PIK_CONTROL_FRAME_PONG:
        if (len != 0) {
            LOG("bad PONG len=%zu", len);
            close_link();
            return false;
        }
        return true;
    case PIK_CONTROL_FRAME_COMMAND:
        if (len != 5) {
            LOG("bad COMMAND len=%zu", len);
            close_link();
            return false;
        }
        {
            pik_control_action_t action = (pik_control_action_t)p[4];
            uint32_t request_id = pik_get_u32le(p);
            if (!action_valid(action)) {
                LOG("rejecting unknown command action=%u request=%u", p[4], request_id);
                return pik_control_send_ack(request_id, PIK_CONTROL_ACK_UNKNOWN_ACTION,
                                            NULL, 0);
            }
            if (g_ctrl.on_command)
                g_ctrl.on_command(action, request_id, g_ctrl.ctx);
            if (g_ctrl.failed)
                return false;
        }
        return true;
    case PIK_CONTROL_FRAME_ACK:
        if (len < 5) {
            LOG("bad ACK len=%zu", len);
            close_link();
            return false;
        }
        g_ctrl.ack_request_id = pik_get_u32le(p);
        g_ctrl.ack_status = (pik_control_ack_status_t)p[4];
        if (!ack_status_valid(g_ctrl.ack_status)) {
            LOG("bad ACK status=%u request=%u", p[4], g_ctrl.ack_request_id);
            close_link();
            return false;
        }
        g_ctrl.ack_payload_len = len - 5;
        if (g_ctrl.ack_payload_len > sizeof(g_ctrl.ack_payload))
            g_ctrl.ack_payload_len = sizeof(g_ctrl.ack_payload);
        if (g_ctrl.ack_payload_len)
            memcpy(g_ctrl.ack_payload, p + 5, g_ctrl.ack_payload_len);
        g_ctrl.ack_pending = true;
        return true;
    case PIK_CONTROL_FRAME_LINK_STATE:
        if (len != 4) {
            LOG("bad LINK_STATE len=%zu", len);
            close_link();
            return false;
        }
        g_ctrl.peer_link_flags = pik_get_u32le(p);
        g_ctrl.peer_link_known = true;
        LOG("peer data links: serial=%s tcp=%s",
            (g_ctrl.peer_link_flags & PIK_CONTROL_LINK_SERIAL) ? "up" : "down",
            (g_ctrl.peer_link_flags & PIK_CONTROL_LINK_TCP) ? "up" : "down");
        return true;
    case PIK_CONTROL_FRAME_CONFIG:
        return handle_config(p, len);
    default:
        LOG("bad control frame type=0x%02x len=%zu", type, len);
        close_link();
        return false;
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
        g_rx_reads++;
        g_rx_bytes += (uint64_t)n;
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
        g_tx_writes++;
        g_tx_bytes += (uint64_t)w;
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
    if (tty_flush_io(fd) < 0) {
        LOG("termios flush failed on %s: %s", dev, strerror(errno));
        close(fd);
        g_ctrl.failed = true;
        return false;
    }

    g_now_ms = now;
    g_ctrl.fd = fd;
    g_ctrl.ready = false;
    g_ctrl.failed = false;
    g_ctrl.epev = EPOLLIN;
    g_ctrl.tx_head = g_ctrl.tx_tail = 0;
    g_ctrl.rxbuf_len = 0;
    g_ctrl.tx_session = pik_session_id(now);
    g_ctrl.rx_session = 0;
    g_ctrl.tx_seq = g_ctrl.rx_seq = 0;
    g_ctrl.hist_first = g_ctrl.hist_count = 0;
    g_ctrl.hist_head = g_ctrl.hist_tail = 0;
    g_ctrl.last_resend_ms = 0;
    g_ctrl.gap_since_ms = 0;
    g_ctrl.last_nak_ms = 0;
    g_ctrl.gap_discards = g_ctrl.dup_discards = 0;
    g_ctrl.opened_ms = now;
    g_ctrl.stale_discards = 0;
    g_ctrl.config_sent = false;
    g_ctrl.last_rx_ms = g_ctrl.last_tx_ms = now;
    pik_epoll_set(g_epfd, fd, EPOLLIN, &g_ctrl_tag);
    LOG("link opened: %s", dev);
    return send_hello();
}

bool pik_control_owns_event(void *ptr) {
    return ptr == &g_ctrl_tag;
}

bool pik_control_dispatch(void *ptr, uint32_t events, int64_t now) {
    if (!pik_control_owns_event(ptr)) return true;
    g_now_ms = now;
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
    g_now_ms = now;
    if (g_ctrl.fd < 0 || g_ctrl.failed) return false;
    if (avail())
        drain_tx(now);
    if (g_ctrl.gap_since_ms) {
        if (now - g_ctrl.gap_since_ms > GAP_BUDGET_MS) {
            LOG("link failure: seq gap not healed expected=%u after %dms discarded=%u",
                g_ctrl.rx_seq, GAP_BUDGET_MS, g_ctrl.gap_discards);
            close_link();
            return false;
        }
        if (now - g_ctrl.last_nak_ms >= NAK_RETRY_MS)
            send_nak(now);
        if (g_ctrl.failed) return false;
    }
    if (!g_ctrl.ready) {
        if (!avail() && (now - g_ctrl.last_tx_ms) > HELLO_RETRY_MS)
            send_hello();
        if ((now - g_ctrl.last_rx_ms) > LINK_DEAD_MS) {
            LOG("handshake RX timeout");
            close_link();
            return false;
        }
    } else {
        if (!g_ctrl.config_sent)
            send_config();
        if (!avail() && (now - g_ctrl.last_tx_ms) > PING_IDLE_MS)
            enqueue_frame(PIK_CONTROL_FRAME_PING, NULL, 0);
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
    int64_t a, b;
    if (!g_ctrl.ready) {
        a = g_ctrl.last_tx_ms + HELLO_RETRY_MS;
        b = g_ctrl.last_rx_ms + LINK_DEAD_MS;
    } else {
        a = g_ctrl.last_tx_ms + PING_IDLE_MS;
        b = g_ctrl.last_rx_ms + LINK_DEAD_MS;
    }
    int64_t dl = a < b ? a : b;
    if (g_ctrl.gap_since_ms) {
        int64_t c = g_ctrl.gap_since_ms + GAP_BUDGET_MS;
        int64_t d = g_ctrl.last_nak_ms + NAK_RETRY_MS;
        if (c < dl) dl = c;
        if (d < dl) dl = d;
    }
    return dl;
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
    g_ctrl.tx_session = 0;
    g_ctrl.rx_session = 0;
    g_ctrl.tx_seq = g_ctrl.rx_seq = 0;
    g_ctrl.config_sent = false;
    clear_pending_ack();
    g_ctrl.peer_link_known = false;
    g_ctrl.peer_link_flags = 0;
}

void pik_control_set_config(const uint8_t *channels, size_t n_channels,
                            pik_control_tcp_role_t tcp_role) {
    if (n_channels > sizeof(g_ctrl.local_channels))
        n_channels = sizeof(g_ctrl.local_channels);
    if (n_channels)
        memcpy(g_ctrl.local_channels, channels, n_channels);
    g_ctrl.local_channel_count = n_channels;
    g_ctrl.local_tcp_role = tcp_role;
    g_ctrl.config_sent = false;
    if (g_ctrl.ready)
        send_config();
}

bool pik_control_send_command(pik_control_action_t action, uint32_t *request_id) {
    if (!g_ctrl.ready) return false;
    uint8_t p[5];
    uint32_t id = g_ctrl.next_request_id++;
    if (g_ctrl.next_request_id == 0) g_ctrl.next_request_id = 1;
    pik_put_u32le(p, id);
    p[4] = (uint8_t)action;
    if (!enqueue_frame(PIK_CONTROL_FRAME_COMMAND, p, sizeof(p))) return false;
    if (request_id) *request_id = id;
    LOG("sent command %s request=%u", pik_control_action_name(action), id);
    return true;
}

bool pik_control_take_ack(uint32_t *request_id, pik_control_ack_status_t *status,
                          const uint8_t **payload, size_t *payload_len) {
    if (!g_ctrl.ack_pending) return false;
    if (request_id) *request_id = g_ctrl.ack_request_id;
    if (status) *status = g_ctrl.ack_status;
    if (payload) *payload = g_ctrl.ack_payload;
    if (payload_len) *payload_len = g_ctrl.ack_payload_len;
    g_ctrl.ack_pending = false;
    return true;
}

bool pik_control_send_ack(uint32_t request_id, pik_control_ack_status_t status,
                          const uint8_t *payload, size_t payload_len) {
    uint8_t p[PIK_CONTROL_MAX_PAYLOAD];
    if (payload_len > sizeof(p) - 5)
        payload_len = sizeof(p) - 5;
    pik_put_u32le(p, request_id);
    p[4] = status;
    if (payload_len)
        memcpy(p + 5, payload, payload_len);
    return enqueue_frame(PIK_CONTROL_FRAME_ACK, p, 5 + payload_len);
}

bool pik_control_send_link_state(uint32_t flags) {
    uint8_t p[4];
    pik_put_u32le(p, flags);
    return enqueue_frame(PIK_CONTROL_FRAME_LINK_STATE, p, sizeof(p));
}

bool pik_control_peer_link_state(uint32_t *flags) {
    if (!g_ctrl.peer_link_known) return false;
    if (flags) *flags = g_ctrl.peer_link_flags;
    return true;
}
