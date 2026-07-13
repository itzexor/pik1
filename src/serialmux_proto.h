#pragma once

enum {
    PIK_SERIALMUX_FRAME_DATA  = 0x01u,
    PIK_SERIALMUX_FRAME_FLUSH = 0x02u,
    PIK_SERIALMUX_FRAME_READY = 0x03u,
    /* Link-control frame, outside the sequenced stream: payload is the 2-byte
     * LE seq the receiver expects next; the sender retransmits from there. */
    PIK_SERIALMUX_FRAME_NAK   = 0x04u,
};

enum {
    PIK_SERIALMUX_MAX_PAYLOAD      = 4096u,
    PIK_SERIALMUX_FRAME_HEADER_LEN = 8u,
};
