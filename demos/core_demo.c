/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 EPFL Spacecraft Team
 */

/* Minimal CHIPS usage: encode a packet, then decode it from a byte stream. */

#include <stdio.h>

#include "chips/core.h"

int main(void) {
    chips_crc16_kermit_init(); /* once, before anything else */

    /* Encode a request (cmd 0x10) into a wire frame. */
    static const uint8_t payload[] = {0x01, 0x7E, 0x02};
    static uint8_t wire[CHIPS_WIRE_MAX];
    size_t wire_len = chips_encode(1, CHIPS_DIR_REQUEST, 0x10, payload, sizeof payload, wire);

    /* Decode: feed received bytes, loop until all are consumed. */
    static uint8_t buf[CHIPS_MAX_PAYLOAD];
    chips_decoder_t dec;
    chips_decoder_init(&dec, buf);

    const uint8_t* in = wire;
    size_t len = wire_len;
    while (len > 0) {
        size_t consumed;
        chips_frame_t frame;
        chips_dec_status_t st = chips_decode(&dec, in, len, &consumed, &frame);
        in += consumed;
        len -= consumed;

        if (st == CHIPS_DEC_FRAME) {
            printf("frame: seq=%u cmd=0x%02X len=%zu\n", frame.seq, frame.cmd, frame.payload_len);
        } else if (st != CHIPS_DEC_NEED_MORE) {
            printf("decode error %d\n", (int)st);
        }
    }
    return 0;
}
