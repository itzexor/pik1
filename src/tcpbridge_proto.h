#pragma once

enum {
    PIK_TCPBRIDGE_FRAME_OPEN   = 0x20u,
    PIK_TCPBRIDGE_FRAME_DATA   = 0x21u,
    PIK_TCPBRIDGE_FRAME_CLOSE  = 0x22u,
    PIK_TCPBRIDGE_FRAME_PAUSE  = 0x23u,
    PIK_TCPBRIDGE_FRAME_RESUME = 0x24u,
    /* Link-control frame, outside the sequenced stream: payload is the 2-byte
     * LE seq the receiver expects next; the sender retransmits from there.
     * Header session is the session being healed (the sender's TX session). */
    PIK_TCPBRIDGE_FRAME_NAK    = 0x25u,
};

enum {
    PIK_TCPBRIDGE_MAX_PAYLOAD      = 2032u,
    PIK_TCPBRIDGE_FRAME_HEADER_LEN = 8u,
};
