#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "frame.h"

/* Unified single-link wire protocol.
 *
 * All traffic shares one sequenced link. Every frame carries the 8-byte
 * header [type:1][ch:1][session_le:4][seq_le:2]; the channel byte selects
 * the service:
 *
 *   ch 0        control (handshake, liveness, peer commands)
 *   ch 1..8     serial mux data channels (wire id is derived from CLI id)
 *   ch 9..14    reserved
 *   ch 15       TCP tunnel
 *
 * Frame types are namespaced by their high nibble so the router can
 * validate type/channel agreement: 0x0x mux, 0x1x control, 0x2x tunnel.
 * The NAK link-control frame sits outside the sequenced stream and outside
 * the service namespaces. */

enum {
    PIK_CH_CONTROL   = 0u,
    PIK_MUX_CLI_BASE = 0u,
    PIK_MUX_CLI_LAST = 7u,
    PIK_MUX_CLI_COUNT = 8u,
    PIK_CH_DATA_BASE = 1u,
    PIK_CH_DATA_LAST = 8u,
    PIK_CH_TUNNEL    = 15u,
};

static inline uint8_t pik_mux_cli_to_wire(uint8_t cli_id) {
    return (uint8_t)(PIK_CH_DATA_BASE + cli_id - PIK_MUX_CLI_BASE);
}

static inline uint8_t pik_mux_wire_to_cli(uint8_t wire_id) {
    return (uint8_t)(PIK_MUX_CLI_BASE + wire_id - PIK_CH_DATA_BASE);
}

static inline bool pik_mux_cli_valid(uint8_t cli_id) {
    return cli_id <= PIK_MUX_CLI_LAST;
}

static inline bool pik_mux_wire_valid(uint8_t wire_id) {
    return wire_id >= PIK_CH_DATA_BASE && wire_id <= PIK_CH_DATA_LAST;
}

enum {
    /* serial mux (ch 1..8) */
    PIK_FRAME_MUX_DATA      = 0x01u,
    PIK_FRAME_MUX_FLUSH     = 0x02u,
    PIK_FRAME_MUX_READY     = 0x03u,

    /* Link-control frame, outside the sequenced stream: payload is the 2-byte
     * LE seq the receiver expects next; the sender retransmits from there.
     * Header session is the session being healed (the sender's TX session). */
    PIK_FRAME_NAK           = 0x0fu,

    /* control (ch 0) */
    PIK_FRAME_CTRL_HELLO      = 0x10u,
    PIK_FRAME_CTRL_PING       = 0x11u,
    PIK_FRAME_CTRL_PONG       = 0x12u,
    PIK_FRAME_CTRL_COMMAND    = 0x13u,
    PIK_FRAME_CTRL_ACK        = 0x14u,
    PIK_FRAME_CTRL_CONFIG        = 0x15u,

    /* TCP tunnel (ch 15); payload starts with [conn:1][gen:1] */
    PIK_FRAME_TUN_OPEN   = 0x20u,
    PIK_FRAME_TUN_DATA   = 0x21u,
    PIK_FRAME_TUN_CLOSE  = 0x22u,
    PIK_FRAME_TUN_PAUSE  = 0x23u,
    PIK_FRAME_TUN_RESUME = 0x24u,
};

enum {
    PIK_FRAME_HEADER_LEN = 8u,

    PIK_MUX_MAX_PAYLOAD  = PIK_WIRE_MAX_PAYLOAD,

    PIK_CTRL_MAX_PAYLOAD = 128u,
    PIK_CTRL_ACK_MAX_PAYLOAD = PIK_CTRL_MAX_PAYLOAD - 5u,
    PIK_CTRL_HELLO_LEN   = 25u,
    PIK_CTRL_RELEASE_LEN = 16u,

    /* tunnel: [conn:1][gen:1] prefix, then at most PIK_TUN_MAX_DATA bytes.
     * Data frames stay small so a tunnel burst queued ahead of an MCU frame
     * costs little wire time on the shared link. */
    PIK_TUN_PREFIX_LEN   = 2u,
    PIK_TUN_MAX_DATA     = 1024u,
    PIK_TUN_MAX_PAYLOAD  = PIK_TUN_PREFIX_LEN + PIK_TUN_MAX_DATA,
};

enum {
    PIK_CTRL_HELLO_MAGIC_0 = 'P',
    PIK_CTRL_HELLO_MAGIC_1 = 'I',
    PIK_CTRL_HELLO_MAGIC_2 = 'K',
    PIK_CTRL_HELLO_MAGIC_3 = '1',
};
