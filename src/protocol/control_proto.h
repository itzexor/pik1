#pragma once

enum {
    PIK_CONTROL_FRAME_HELLO      = 0x01u,
    PIK_CONTROL_FRAME_PING       = 0x02u,
    PIK_CONTROL_FRAME_PONG       = 0x03u,
    PIK_CONTROL_FRAME_COMMAND    = 0x04u,
    PIK_CONTROL_FRAME_ACK        = 0x05u,
    PIK_CONTROL_FRAME_LINK_STATE = 0x06u,
    PIK_CONTROL_FRAME_CONFIG     = 0x07u,
    /* Link-control frame, outside the sequenced stream: payload is the 2-byte
     * LE seq the receiver expects next; the sender retransmits from there.
     * Header session is the session being healed (the sender's TX session). */
    PIK_CONTROL_FRAME_NAK        = 0x08u,
};

enum {
    PIK_CONTROL_HELLO_MAGIC_0 = 'P',
    PIK_CONTROL_HELLO_MAGIC_1 = 'I',
    PIK_CONTROL_HELLO_MAGIC_2 = 'K',
    PIK_CONTROL_HELLO_MAGIC_3 = '1',
};

enum {
    PIK_CONTROL_HELLO_LEN        = 29u,
    PIK_CONTROL_RELEASE_LEN      = 16u,
    PIK_CONTROL_MAX_PAYLOAD      = 128u,
    PIK_CONTROL_FRAME_HEADER_LEN = 7u,
};
