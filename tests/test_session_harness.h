#pragma once

/* Shared fixture for session-level protocol tests: brings up the full service
 * stack (session + control + mux + tunnel) and lets the test act as the peer
 * by feeding/draining encoded frames directly at the link boundary. */

#include "test_harness.h"

#include "control.h"
#include "link.h"
#include "pik_proto.h"
#include "serialmux.h"
#include "session.h"
#include "tunnel.h"
#include "product.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>

typedef struct {
    int epfd;
    uint32_t peer_session;
    uint32_t local_session;   /* the daemon's TX session, learned from frames */
    uint16_t peer_seq;
    uint8_t txbuf[8192];
    size_t txbuf_len;
} session_fixture_t;

static TEST_UNUSED int sfx_ready_calls;

static TEST_UNUSED void sfx_put_u16le(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static TEST_UNUSED bool sfx_feed_encoded(const uint8_t *enc, size_t enc_len) {
    pik_link_t *lk = pik_session_link();
    if (!pik_link_is_open(lk))
        return false;
    (void)pik_link_feed(lk, enc, enc_len, pik_now_ms());
    return true;
}

/* Mirrors pik1d's on_control_ready: start the data services. */
static TEST_UNUSED void sfx_on_ready(void) {
    sfx_ready_calls++;
    serialmux_start(pik_now_ms());
    tunnel_start(pik_now_ms());
}

typedef struct {
    pik_control_role_t role;
    const serialmux_config_t *mux_cfg;   /* NULL: no channels */
    tunnel_mode_t tunnel_mode;
    const char *tunnel_host;
    int tunnel_port;
    pik_control_command_fn on_command;
    bool stale_input;                    /* write garbage before opening */
} session_fixture_cfg_t;

static TEST_UNUSED bool sfx_write_frame_hdr(session_fixture_t *fx, uint8_t type,
                                            uint8_t ch, uint32_t session,
                                            uint16_t seq, const uint8_t *payload,
                                            size_t payload_len) {
    uint8_t header[PIK_FRAME_HEADER_LEN];
    uint8_t enc[8192];
    size_t enc_len = 0;
    header[0] = type;
    header[1] = ch;
    pik_put_u32le(header + 2, session);
    sfx_put_u16le(header + 6, seq);
    if (!test_encode_frame(header, payload, payload_len,
                           enc, sizeof(enc), &enc_len))
        return false;
    (void)fx;
    return sfx_feed_encoded(enc, enc_len);
}

static TEST_UNUSED bool sfx_send_peer_frame(session_fixture_t *fx, uint8_t type,
                                            uint8_t ch, const uint8_t *payload,
                                            size_t payload_len) {
    return sfx_write_frame_hdr(fx, type, ch, fx->peer_session, fx->peer_seq++,
                               payload, payload_len);
}

/* a NAK carries the session it is healing: the daemon's TX session */
static TEST_UNUSED bool sfx_send_peer_nak(session_fixture_t *fx, uint16_t expected) {
    uint8_t payload[2] = { (uint8_t)expected, (uint8_t)(expected >> 8) };
    return sfx_write_frame_hdr(fx, PIK_FRAME_NAK, 0, fx->local_session,
                               fx->peer_seq /* link-control: seq not consumed */,
                               payload, sizeof(payload));
}

static TEST_UNUSED bool sfx_init(session_fixture_t *fx,
                                 const session_fixture_cfg_t *cfg) {
    memset(fx, 0, sizeof(*fx));
    sfx_ready_calls = 0;
    fx->epfd = epoll_create1(EPOLL_CLOEXEC);
    fx->peer_session = 0xa5c30001u;
    fx->peer_seq = 0;

    if (fx->epfd < 0) return false;

    pik_session_init();
    pik_control_init(cfg->role, cfg->on_command, sfx_on_ready);

    serialmux_config_t empty;
    memset(&empty, 0, sizeof(empty));
    serialmux_init(cfg->mux_cfg ? cfg->mux_cfg : &empty, fx->epfd);
    tunnel_init(fx->epfd, cfg->tunnel_mode,
                cfg->tunnel_host ? cfg->tunnel_host : "",
                cfg->tunnel_port);
    if (cfg->mux_cfg) {
        uint8_t ids[MAX_CHANNELS];
        for (int i = 0; i < cfg->mux_cfg->n_channels; i++)
            ids[i] = cfg->mux_cfg->channels[i].ch_id;
        pik_control_set_config(ids, (size_t)cfg->mux_cfg->n_channels,
                               cfg->tunnel_mode == TUNNEL_MODE_LISTEN
                                   ? PIK_CONTROL_TCP_LISTEN
                                   : cfg->tunnel_mode == TUNNEL_MODE_FORWARD
                                         ? PIK_CONTROL_TCP_FORWARD
                                   : PIK_CONTROL_TCP_NONE);
    }

    pik_link_begin(pik_session_link(), pik_now_ms());

    if (cfg->stale_input) {
        session_fixture_t stale = *fx;
        stale.peer_session = 0x0badcafeu;
        stale.peer_seq = 1;
        uint8_t payload[] = { PIK_CONTROL_TCP_NONE, 0 };
        if (!sfx_write_frame_hdr(&stale, PIK_FRAME_CTRL_CONFIG, PIK_CH_CONTROL,
                                 stale.peer_session, stale.peer_seq,
                                 payload, sizeof(payload)))
            return false;
    }

    return pik_control_on_link_open();
}

static TEST_UNUSED void sfx_cleanup(session_fixture_t *fx) {
    tunnel_cleanup();
    serialmux_cleanup();
    pik_control_cleanup();
    pik_session_cleanup();
    if (fx->epfd >= 0) close(fx->epfd);
}

/* Route one epoll event the way pik1d's main loop does. */
static TEST_UNUSED bool sfx_dispatch(void *ptr, uint32_t events, int64_t now) {
    (void)now;
    if (serialmux_owns_event(ptr))
        return serialmux_dispatch(ptr, events, now);
    if (tunnel_owns_event(ptr))
        return tunnel_dispatch(ptr, events);
    return true;
}

static TEST_UNUSED bool sfx_tick(int64_t now);
static TEST_UNUSED bool sfx_drain_link_tx(session_fixture_t *fx, int64_t now);

static TEST_UNUSED bool sfx_dispatch_one(session_fixture_t *fx, int timeout_ms) {
    size_t tx_before = fx->txbuf_len;
    int64_t now = pik_now_ms();
    if (!sfx_tick(now)) return false;
    if (!sfx_drain_link_tx(fx, now)) return false;
    if (fx->txbuf_len != tx_before)
        return true;

    int64_t end = pik_now_ms() + timeout_ms;
    for (;;) {
        now = pik_now_ms();
        if (!sfx_tick(now)) return false;
        if (!sfx_drain_link_tx(fx, now)) return false;
        if (fx->txbuf_len != tx_before)
            return true;

        struct epoll_event ev;
        int wait_ms = 0;
        if (timeout_ms > 0) {
            int64_t remaining = end - now;
            if (remaining <= 0)
                break;
            wait_ms = remaining < 20 ? (int)remaining : 20;
        }
        int n;
        do {
            n = epoll_wait(fx->epfd, &ev, 1, wait_ms);
        } while (n < 0 && errno == EINTR);
        if (n < 0)
            return false;
        if (n > 0)
            return sfx_dispatch(ev.data.ptr, ev.events, pik_now_ms());
        if (timeout_ms <= 0)
            break;
    }
    return false;
}

static TEST_UNUSED bool sfx_settle(session_fixture_t *fx) {
    int64_t now = pik_now_ms();
    if (!sfx_tick(now)) return false;
    if (!sfx_drain_link_tx(fx, now)) return false;
    return pik_link_is_open(pik_session_link());
}

/* Combined per-loop tick, like pik1d's main loop. */
static TEST_UNUSED bool sfx_tick(int64_t now) {
    if (!pik_control_tick(now)) return false;
    if (!pik_session_tick(now)) return false;
    if (!serialmux_tick(now)) return false;
    if (!tunnel_tick(now)) return false;
    return true;
}

typedef struct {
    uint8_t  type;
    uint8_t  ch;
    uint32_t session;
    uint16_t seq;
    uint8_t  payload[PIK_MUX_MAX_PAYLOAD];
    size_t   payload_len;
    uint8_t  enc[8192];
    size_t   enc_len;
} sfx_frame_t;

static TEST_UNUSED bool sfx_drain_link_tx(session_fixture_t *fx, int64_t now) {
    for (;;) {
        uint32_t len = 0;
        const uint8_t *p = pik_link_tx_peek(pik_session_link(), &len);
        if (!p || len == 0)
            return true;
        if (fx->txbuf_len + len > sizeof(fx->txbuf))
            return false;
        memcpy(fx->txbuf + fx->txbuf_len, p, len);
        fx->txbuf_len += len;
        pik_link_tx_consume(pik_session_link(), len, now);
    }
}

static TEST_UNUSED bool sfx_take_tx_frame(session_fixture_t *fx,
                                          uint8_t *buf, size_t cap,
                                          size_t *len) {
    for (size_t i = 0; i < fx->txbuf_len; i++) {
        if (fx->txbuf[i] != 0)
            continue;
        size_t frame_len = i + 1;
        if (frame_len > cap)
            return false;
        memcpy(buf, fx->txbuf, frame_len);
        memmove(fx->txbuf, fx->txbuf + frame_len, fx->txbuf_len - frame_len);
        fx->txbuf_len -= frame_len;
        *len = frame_len;
        return true;
    }
    return false;
}

/* Read one frame the daemon wrote to the link, dispatching/ticking as needed
 * to let the session scheduler admit queued frames into the link TX FIFO. */
static TEST_UNUSED bool sfx_read_frame(session_fixture_t *fx, sfx_frame_t *f) {
    for (int i = 0; i < 8; i++) {
        int64_t now = pik_now_ms();
        if (!sfx_tick(now)) return false;
        if (!sfx_drain_link_tx(fx, now)) return false;
        if (sfx_take_tx_frame(fx, f->enc, sizeof(f->enc), &f->enc_len)) {
            uint8_t dec[8192];
            pik_frame_t frame;
            if (pik_frame_decode(f->enc, f->enc_len, sizeof(f->enc),
                                 dec, sizeof(dec), &frame) != PIK_FRAME_OK)
                return false;
            f->type = frame.header[0];
            f->ch = frame.header[1];
            f->session = pik_get_u32le(frame.header + 2);
            f->seq = (uint16_t)frame.header[6] | (uint16_t)frame.header[7] << 8;
            f->payload_len = frame.payload_len;
            if (f->payload_len)
                memcpy(f->payload, frame.payload, f->payload_len);
            if (f->type != PIK_FRAME_NAK)
                fx->local_session = f->session;
            return true;
        }
        sfx_dispatch_one(fx, 100);
    }
    return false;
}

static TEST_UNUSED bool sfx_send_peer_hello(session_fixture_t *fx,
                                            pik_control_role_t role,
                                            uint32_t protocol) {
    uint8_t p[PIK_CTRL_HELLO_LEN];
    memset(p, 0, sizeof(p));
    p[0] = PIK_CTRL_HELLO_MAGIC_0;
    p[1] = PIK_CTRL_HELLO_MAGIC_1;
    p[2] = PIK_CTRL_HELLO_MAGIC_2;
    p[3] = PIK_CTRL_HELLO_MAGIC_3;
    pik_put_u32le(p + 4, protocol);
    p[8] = (uint8_t)role;
    snprintf((char *)p + 9, PIK_CTRL_RELEASE_LEN, "%s", PIK1_RELEASE_VERSION);
    return sfx_send_peer_frame(fx, PIK_FRAME_CTRL_HELLO, PIK_CH_CONTROL,
                               p, sizeof(p));
}

/* Complete the handshake acting as the peer (default: peer is the MCU side).
 * Consumes the daemon's HELLO, HELLO response, and CONFIG. */
static TEST_UNUSED bool sfx_handshake(session_fixture_t *fx) {
    sfx_frame_t f;

    if (!sfx_read_frame(fx, &f)) return false;
    if (f.type != PIK_FRAME_CTRL_HELLO || f.seq != 0 || f.session == 0)
        return false;

    if (!sfx_send_peer_hello(fx, PIK_CONTROL_ROLE_MCU, PIK1_PROTOCOL_VERSION))
        return false;
    if (!sfx_dispatch_one(fx, 1000)) return false;
    if (sfx_ready_calls != 1) return false;

    if (!sfx_read_frame(fx, &f)) return false;
    if (f.type != PIK_FRAME_CTRL_HELLO) return false;

    if (!sfx_read_frame(fx, &f)) return false;
    return f.type == PIK_FRAME_CTRL_CONFIG;
}
