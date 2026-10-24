/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 EPFL Spacecraft Team
 */

#include "chips/core.h"

#include "compiler.h"

#define CRC16_KERMIT_POLYNOMIAL ((uint16_t)0x8408u)

static uint16_t crc16_kermit_table[CHIPS_CRC16_KERMIT_FOLD][256];

static void chips_crc16_kermit_build_base_table(void) {
    unsigned byte;
    for (byte = 0; byte < 256u; ++byte) {
        uint16_t crc = (uint16_t)byte;
        unsigned bit;
        for (bit = 0; bit < 8u; ++bit) {
            if (crc & 1u) {
                crc = (uint16_t)((crc >> 1) ^ CRC16_KERMIT_POLYNOMIAL);
            } else {
                crc = (uint16_t)(crc >> 1);
            }
        }
        crc16_kermit_table[0][byte] = crc;
    }
}

static void chips_crc16_kermit_build_slicing_tables(void) {
    unsigned slice;
    for (slice = 1; slice < CHIPS_CRC16_KERMIT_FOLD; ++slice) {
        unsigned byte;
        for (byte = 0; byte < 256u; ++byte) {
            const uint16_t previous = crc16_kermit_table[slice - 1u][byte];
            const uint16_t index = (uint16_t)(previous & 0xFFu);
            const uint16_t shifted = (uint16_t)(previous >> 8);
            const uint16_t value = crc16_kermit_table[0][index];
            crc16_kermit_table[slice][byte] = (uint16_t)(shifted ^ value);
        }
    }
}

void chips_crc16_kermit_init(void) {
    chips_crc16_kermit_build_base_table();
    chips_crc16_kermit_build_slicing_tables();
}

uint16_t chips_crc16_kermit_compute(const uint8_t* data, size_t length, uint16_t crc) {
#if CHIPS_CRC16_KERMIT_FOLD >= 2
    while (length >= CHIPS_CRC16_KERMIT_FOLD) {
        const uint16_t input = (uint16_t)(data[0] | ((uint16_t)data[1] << 8));
        const uint16_t folded = (uint16_t)(crc ^ input);
        const uint8_t low = (uint8_t)(folded & 0xFFu);
        const uint8_t high = (uint8_t)((folded >> 8) & 0xFFu);
        const uint16_t lut_low = crc16_kermit_table[CHIPS_CRC16_KERMIT_FOLD - 1u][low];
        const uint16_t lut_high = crc16_kermit_table[CHIPS_CRC16_KERMIT_FOLD - 2u][high];

        uint16_t result = (uint16_t)(lut_low ^ lut_high);
        CHIPS_UNROLL_FULL
        for (unsigned i = 2; i < CHIPS_CRC16_KERMIT_FOLD; ++i) {
            result ^= crc16_kermit_table[CHIPS_CRC16_KERMIT_FOLD - 1u - i][data[i]];
        }

        crc = result;
        data += CHIPS_CRC16_KERMIT_FOLD;
        length -= CHIPS_CRC16_KERMIT_FOLD;
    }
#endif

    while (length--) {
        const uint8_t index = (uint8_t)((crc ^ *data++) & 0xFFu);
        crc = (uint16_t)((crc >> 8) ^ crc16_kermit_table[0][index]);
    }

    return crc;
}

#ifdef CHIPS_NO_LIBC_STRING

static const uint8_t* chips_find(const uint8_t* p, const uint8_t* end, uint8_t value) {
    for (; p < end; ++p) {
        if (*p == value) {
            return p;
        }
    }
    return NULL;
}

static void chips_copy(uint8_t* dst, const uint8_t* src, size_t n) {
    while (n-- != 0u) {
        *dst++ = *src++;
    }
}

#else

static const uint8_t* chips_find(const uint8_t* p, const uint8_t* end, uint8_t value) {
    return (const uint8_t*)memchr(p, value, (size_t)(end - p));
}

static void chips_copy(uint8_t* dst, const uint8_t* src, size_t n) {
    (void)memcpy(dst, src, n);
}

#endif

static size_t chips_dist(const uint8_t* p, const uint8_t* end, uint8_t value) {
    const uint8_t* hit;
    if (p == end) {
        return 0u;
    }
    hit = chips_find(p, end, value);
    return (size_t)(((hit != NULL) ? hit : end) - p);
}

static uint8_t* chips_put_stuffed(uint8_t* o, uint8_t b) {
    if (b == CHIPS_SYNC) {
        *o++ = CHIPS_ESC;
        *o++ = CHIPS_SYNC_SUB;
    } else if (b == CHIPS_ESC) {
        *o++ = CHIPS_ESC;
        *o++ = CHIPS_ESC_SUB;
    } else {
        *o++ = b;
    }
    return o;
}

static uint8_t* chips_stuff(uint8_t* output, const uint8_t* input, size_t input_len) {
    const uint8_t* const input_end = input + input_len;

    size_t dist_to_sync = chips_dist(input, input_end, CHIPS_SYNC);
    size_t dist_to_esc = chips_dist(input, input_end, CHIPS_ESC);

    for (;;) {
        const size_t plain_run_len = (dist_to_sync < dist_to_esc) ? dist_to_sync : dist_to_esc;

        if (plain_run_len != 0u) {
            chips_copy(output, input, plain_run_len);
            output += plain_run_len;
            input += plain_run_len;
            dist_to_sync -= plain_run_len;
            dist_to_esc -= plain_run_len;
        }

        if (input == input_end) {
            break;
        }

        *output++ = CHIPS_ESC;
        input++;

        if (dist_to_sync == 0u) {
            *output++ = CHIPS_SYNC_SUB;
            dist_to_sync = chips_dist(input, input_end, CHIPS_SYNC);
            dist_to_esc -= 1u;
        } else {
            *output++ = CHIPS_ESC_SUB;
            dist_to_esc = chips_dist(input, input_end, CHIPS_ESC);
            dist_to_sync -= 1u;
        }
    }

    return output;
}

size_t chips_encode(uint8_t seq,
                    chips_direction_t direction,
                    uint8_t cmd,
                    const uint8_t* payload,
                    size_t payload_len,
                    uint8_t out[CHIPS_WIRE_MAX]) {
    if (out == NULL || payload_len > CHIPS_MAX_PAYLOAD || cmd > CHIPS_CMD_MASK ||
        (payload == NULL && payload_len != 0u)) {
        return 0u;
    }

    uint8_t hdr[CHIPS_HEADER_SIZE];
    hdr[0] = seq;
    hdr[1] = (uint8_t)((direction == CHIPS_DIR_RESPONSE ? CHIPS_FLAG_RESP : 0u) | cmd);

    uint16_t crc = chips_crc16_kermit_compute(hdr, CHIPS_HEADER_SIZE, CHIPS_CRC16_KERMIT_INITIAL);
    if (payload_len != 0u) {
        crc = chips_crc16_kermit_compute(payload, payload_len, crc);
    }

    uint8_t* out_ptr = out;

    *out_ptr++ = CHIPS_SYNC;

    out_ptr = chips_put_stuffed(out_ptr, hdr[0]);
    out_ptr = chips_put_stuffed(out_ptr, hdr[1]);

    if (payload_len != 0u) {
        out_ptr = chips_stuff(out_ptr, payload, payload_len);
    }

    out_ptr = chips_put_stuffed(out_ptr, (uint8_t)(crc & 0xFFu));
    out_ptr = chips_put_stuffed(out_ptr, (uint8_t)(crc >> 8));

    *out_ptr++ = CHIPS_SYNC;

    return (size_t)(out_ptr - out);
}

#define CHIPS_BODY_END (CHIPS_HEADER_SIZE + CHIPS_MAX_PAYLOAD)

static uint8_t chips_dec_body_at(const chips_decoder_t* dec, size_t i) {
    CHIPS_ASSERT(i < CHIPS_FRAME_MAX - CHIPS_HEADER_SIZE);
    return (i < CHIPS_MAX_PAYLOAD) ? dec->payload[i] : dec->spill[i - CHIPS_MAX_PAYLOAD];
}

static int chips_dec_store_byte(chips_decoder_t* dec, uint8_t b) {
    const size_t pos = dec->len;

    if (pos >= CHIPS_FRAME_MAX) {
        return 0;
    }

    dec->len = pos + 1u;

    if (pos < CHIPS_HEADER_SIZE) {
        dec->hdr[pos] = b;
    } else if (pos < CHIPS_BODY_END) {
        dec->payload[pos - CHIPS_HEADER_SIZE] = b;
    } else {
        dec->spill[pos - CHIPS_BODY_END] = b;
    }

    return 1;
}

static int chips_dec_store(chips_decoder_t* dec, const uint8_t* src, size_t n) {
    size_t pos = dec->len;

    if (n > (size_t)CHIPS_FRAME_MAX - pos) {
        return 0;
    }
    dec->len = pos + n;

    while (n != 0u && pos < CHIPS_HEADER_SIZE) {
        dec->hdr[pos++] = *src++;
        n--;
    }

    if (n != 0u && pos < CHIPS_BODY_END) {
        const size_t room = CHIPS_BODY_END - pos;
        const size_t c = (n < room) ? n : room;
        chips_copy(dec->payload + (pos - CHIPS_HEADER_SIZE), src, c);
        pos += c;
        src += c;
        n -= c;
    }

    while (n != 0u) {
        dec->spill[pos++ - CHIPS_BODY_END] = *src++;
        n--;
    }

    return 1;
}

static int chips_unescape(uint8_t* b) {
    if (*b == CHIPS_SYNC_SUB) {
        *b = CHIPS_SYNC;
        return 1;
    }
    if (*b == CHIPS_ESC_SUB) {
        *b = CHIPS_ESC;
        return 1;
    }
    return 0;
}

static void chips_dec_open(chips_decoder_t* dec) {
    dec->len = 0u;
    dec->state = CHIPS_RX_DATA;
}

static chips_dec_status_t chips_dec_close(const chips_decoder_t* dec, chips_frame_t* frame) {
    if (dec->state == CHIPS_RX_ESCAPED) {
        return CHIPS_DEC_ERR_ESCAPE;
    }

    if (dec->len == 0u) {
        return CHIPS_DEC_NEED_MORE;
    }

    if (dec->len < CHIPS_FRAME_MIN) {
        return CHIPS_DEC_ERR_TOO_SHORT;
    }

    const size_t payload_len = dec->len - CHIPS_FRAME_MIN;

    const uint8_t rx_crc_low = chips_dec_body_at(dec, payload_len);
    const uint8_t rx_crc_high = chips_dec_body_at(dec, payload_len + 1u);
    const uint16_t rx_crc = (uint16_t)(rx_crc_low | ((uint16_t)rx_crc_high << 8));

    uint16_t crc = chips_crc16_kermit_compute(dec->hdr, CHIPS_HEADER_SIZE, CHIPS_CRC16_KERMIT_INITIAL);
    if (payload_len != 0u) {
        crc = chips_crc16_kermit_compute(dec->payload, payload_len, crc);
    }

    if (crc != rx_crc) {
        return CHIPS_DEC_ERR_CRC;
    }

    frame->seq = dec->hdr[0];
    frame->direction = (dec->hdr[1] & CHIPS_FLAG_RESP) ? CHIPS_DIR_RESPONSE : CHIPS_DIR_REQUEST;
    frame->cmd = (uint8_t)(dec->hdr[1] & CHIPS_CMD_MASK);
    frame->payload = dec->payload;
    frame->payload_len = payload_len;

    return CHIPS_DEC_FRAME;
}

void chips_decoder_init(chips_decoder_t* dec, uint8_t payload_buf[CHIPS_MAX_PAYLOAD]) {
    if (dec == NULL) {
        return;
    }
    dec->payload = payload_buf;
    dec->len = 0;
    (void)chips_decoder_reset(dec);
}

size_t chips_decoder_reset(chips_decoder_t* dec) {
    if (dec == NULL) {
        return 0;
    }
    size_t len = dec->len;
    dec->len = 0u;
    dec->state = CHIPS_RX_HUNT;
    return len;
}

#define CHIPS_DEC_CONTINUE CHIPS_DEC_NEED_MORE

typedef struct {
    const uint8_t* p;
    const uint8_t* end;
} chips_cursor_t;

static int chips_dec_args_ok(const chips_decoder_t* dec,
                             const uint8_t* in,
                             size_t in_len,
                             const size_t* consumed,
                             const chips_frame_t* frame) {
    return dec != NULL && dec->payload != NULL && consumed != NULL && frame != NULL && (in != NULL || in_len == 0u);
}

static chips_dec_status_t chips_dec_fail(chips_decoder_t* dec, chips_dec_status_t st) {
    dec->len = 0u;
    dec->state = CHIPS_RX_HUNT;
    return st;
}

static chips_dec_status_t chips_dec_put_run(chips_decoder_t* dec, const uint8_t* src, size_t n) {
    if (chips_dec_store(dec, src, n)) {
        return CHIPS_DEC_CONTINUE;
    }
    return chips_dec_fail(dec, CHIPS_DEC_ERR_TOO_LONG);
}

static chips_dec_status_t chips_dec_put_escaped(chips_decoder_t* dec, uint8_t sub) {
    uint8_t b = sub;

    dec->state = CHIPS_RX_DATA;
    if (!chips_unescape(&b)) {
        return chips_dec_fail(dec, CHIPS_DEC_ERR_ESCAPE);
    }
    if (!chips_dec_store_byte(dec, b)) {
        return chips_dec_fail(dec, CHIPS_DEC_ERR_TOO_LONG);
    }
    return CHIPS_DEC_CONTINUE;
}

static void chips_dec_hunt(chips_decoder_t* dec, chips_cursor_t* cur) {
    const uint8_t* s = chips_find(cur->p, cur->end, CHIPS_SYNC);

    if (s == NULL) {
        cur->p = cur->end;
        return;
    }
    cur->p = s + 1;
    chips_dec_open(dec);
}

static chips_dec_status_t chips_dec_unstuff_step(chips_decoder_t* dec, chips_cursor_t* cur, const uint8_t* win_end) {
    const uint8_t* esc = chips_find(cur->p, win_end, CHIPS_ESC);
    if (esc == NULL) {
        esc = win_end;
    }

    const chips_dec_status_t st = chips_dec_put_run(dec, cur->p, (size_t)(esc - cur->p));
    cur->p = esc;
    if (st != CHIPS_DEC_CONTINUE) {
        return st;
    }

    if (esc == win_end) {
        return CHIPS_DEC_CONTINUE;
    }

    cur->p = esc + 1;
    if (cur->p == win_end) {
        dec->state = (uint8_t)CHIPS_RX_ESCAPED;
        return CHIPS_DEC_CONTINUE;
    }
    return chips_dec_put_escaped(dec, *cur->p++);
}

static chips_dec_status_t chips_dec_unstuff(chips_decoder_t* dec, chips_cursor_t* cur, const uint8_t* win_end) {
    chips_dec_status_t st = CHIPS_DEC_CONTINUE;

    if (dec->state == CHIPS_RX_ESCAPED && cur->p != win_end) {
        st = chips_dec_put_escaped(dec, *cur->p++);
    }
    while (st == CHIPS_DEC_CONTINUE && cur->p != win_end) {
        st = chips_dec_unstuff_step(dec, cur, win_end);
    }
    return st;
}

static chips_dec_status_t chips_dec_window(chips_decoder_t* dec, chips_cursor_t* cur, chips_frame_t* frame) {
    const uint8_t* win_end = chips_find(cur->p, cur->end, CHIPS_SYNC);
    if (win_end == NULL) {
        win_end = cur->end;
    }

    chips_dec_status_t st = chips_dec_unstuff(dec, cur, win_end);
    if (st != CHIPS_DEC_CONTINUE || win_end == cur->end) {
        return st;
    }

    cur->p = win_end + 1;
    st = chips_dec_close(dec, frame);
    chips_dec_open(dec);
    return st;
}

static chips_dec_status_t chips_dec_step(chips_decoder_t* dec, chips_cursor_t* cur, chips_frame_t* frame) {
    if (dec->state == CHIPS_RX_HUNT) {
        chips_dec_hunt(dec, cur);
        return CHIPS_DEC_CONTINUE;
    }
    return chips_dec_window(dec, cur, frame);
}

chips_dec_status_t chips_decode(chips_decoder_t* dec,
                                const uint8_t* in,
                                size_t in_len,
                                size_t* consumed,
                                chips_frame_t* frame) {
    if (consumed != NULL) {
        *consumed = 0u;
    }
    if (!chips_dec_args_ok(dec, in, in_len, consumed, frame)) {
        return CHIPS_DEC_ERR_ARG;
    }
    if (in_len == 0u) {
        return CHIPS_DEC_NEED_MORE;
    }

    chips_cursor_t cur = {in, in + in_len};
    chips_dec_status_t st = CHIPS_DEC_CONTINUE;

    while (st == CHIPS_DEC_CONTINUE && cur.p != cur.end) {
        st = chips_dec_step(dec, &cur, frame);
    }

    *consumed = (size_t)(cur.p - in);
    return st;
}
