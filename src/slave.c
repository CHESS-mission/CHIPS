/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 EPFL Spacecraft Team
 */

#include "chips/slave.h"

static uint32_t elapsed_us(uint32_t now, uint32_t since) {
    return (uint32_t)(now - since);
}

static bool tx_in_flight(const chips_slave_t* s) {
    return s->tx_off < s->tx_len;
}

static void tx_abort(chips_slave_t* s) {
    s->tx_off = s->tx_len;
}

static chips_slave_fingerprint_t fingerprint(const chips_frame_t* f) {
    chips_slave_fingerprint_t fp;
    fp.seq = f->seq;
    fp.cmd = f->cmd;
    fp.crc = f->crc;
    return fp;
}

static bool same_request(const chips_slave_fingerprint_t* a, const chips_slave_fingerprint_t* b) {
    return a->seq == b->seq && a->cmd == b->cmd && a->crc == b->crc;
}

static void expire_cache(chips_slave_t* s) {
    if (s->cache_valid && elapsed_us(s->now, s->t_cached) >= CHIPS_CACHE_WINDOW_US) {
        s->cache_valid = false;
    }
}

static uint32_t deliver(chips_slave_t* s, const chips_slave_fingerprint_t* fp) {
    CHIPS_ASSERT(s->frame.payload == s->rx_payload[s->rx_active]);

    s->request.seq = s->frame.seq;
    s->request.cmd = s->frame.cmd;
    s->request.payload = s->frame.payload;
    s->request.payload_len = s->frame.payload_len;

    s->rx_active ^= 1u;
    chips_decoder_set_buffer(&s->dec, s->rx_payload[s->rx_active]);

    s->pending = *fp;
    s->state = CHIPS_SLAVE_PENDING;
    return CHIPS_SLAVE_EV_REQUEST;
}

static uint32_t on_frame(chips_slave_t* s) {
    if (s->frame.direction != CHIPS_DIR_REQUEST) {
        return CHIPS_SLAVE_EV_RX_IGNORED;
    }

    if (s->state == CHIPS_SLAVE_PENDING) {
        return CHIPS_SLAVE_EV_RX_IGNORED;
    }

    const chips_slave_fingerprint_t fp = fingerprint(&s->frame);

    if (s->cache_valid && same_request(&fp, &s->cached)) {
        if (tx_in_flight(s)) {
            return CHIPS_SLAVE_EV_RX_IGNORED;
        }
        s->tx_off = 0;
        return CHIPS_SLAVE_EV_RETRANSMIT;
    }

    return deliver(s, &fp);
}

static uint32_t receive(chips_slave_t* s) {
    uint32_t ev = CHIPS_SLAVE_EV_NONE;
    size_t n = 0;

    if (s->port.read(s->port.ctx, s->rx_chunk, sizeof(s->rx_chunk), &n) != CHIPS_IO_OK) {
        (void)chips_decoder_reset(&s->dec);
        return CHIPS_SLAVE_EV_RX_ERROR;
    }
    CHIPS_ASSERT(n <= sizeof(s->rx_chunk));
    if (n > sizeof(s->rx_chunk)) {
        n = sizeof(s->rx_chunk);
    }

    const uint8_t* p = s->rx_chunk;
    size_t left = n;

    while (left > 0) {
        size_t used = 0;
        const chips_dec_status_t st = chips_decode(&s->dec, p, left, &used, &s->frame);
        CHIPS_ASSERT(used <= left);
        if (used == 0 || used > left) {
            (void)chips_decoder_reset(&s->dec);
            break;
        }
        p += used;
        left -= used;

        switch (st) {
            case CHIPS_DEC_NEED_MORE:
                break;
            case CHIPS_DEC_FRAME:
                ev |= on_frame(s);
                break;
            case CHIPS_DEC_ERR_ESCAPE:
            case CHIPS_DEC_ERR_TOO_LONG:
            case CHIPS_DEC_ERR_TOO_SHORT:
            case CHIPS_DEC_ERR_CRC:
                ev |= CHIPS_SLAVE_EV_RX_BAD_FRAME;
                break;
            case CHIPS_DEC_ERR_ARG:
            default:
                CHIPS_ASSERT(0);
                break;
        }
    }
    return ev;
}

static uint32_t transmit(chips_slave_t* s) {
    if (!tx_in_flight(s)) {
        return CHIPS_SLAVE_EV_NONE;
    }

    const size_t want = s->tx_len - s->tx_off;
    size_t written = 0;
    if (s->port.write(s->port.ctx, &s->tx_frame[s->tx_off], want, &written) != CHIPS_IO_OK) {
        s->tx_off = s->tx_len;
        return CHIPS_SLAVE_EV_TX_ERROR;
    }
    CHIPS_ASSERT(written <= want);
    if (written > want) {
        written = want;
    }

    s->tx_off += written;

    if (written < want) {
        return CHIPS_SLAVE_EV_NONE;
    }
    return CHIPS_SLAVE_EV_TX_FRAME;
}

static void release_request(chips_slave_t* s) {
    s->request = (chips_slave_request_t){0};
    s->state = CHIPS_SLAVE_LISTENING;
}

void chips_slave_init(chips_slave_t* s, const chips_port_t* port) {
    CHIPS_ASSERT(s != NULL && port != NULL);
    CHIPS_ASSERT(port->write != NULL && port->read != NULL && port->now_us != NULL);

    *s = (chips_slave_t){0};
    s->port = *port;
    s->state = CHIPS_SLAVE_LISTENING;
    s->cache_valid = false;
    s->tx_len = 0;
    s->tx_off = 0;
    s->rx_active = 0;
    chips_decoder_init(&s->dec, s->rx_payload[s->rx_active]);
}

uint32_t chips_slave_step(chips_slave_t* s) {
    CHIPS_ASSERT(s != NULL);

    s->now = s->port.now_us(s->port.ctx);
    expire_cache(s);

    uint32_t ev = receive(s);
    ev |= transmit(s);
    return ev;
}

const chips_slave_request_t* chips_slave_request(const chips_slave_t* s) {
    CHIPS_ASSERT(s != NULL);
    return (s->state == CHIPS_SLAVE_PENDING) ? &s->request : NULL;
}

chips_respond_status_t chips_slave_respond(chips_slave_t* s, const uint8_t* payload, size_t payload_len) {
    CHIPS_ASSERT(s != NULL);

    if (s->state != CHIPS_SLAVE_PENDING) {
        return CHIPS_RESPOND_NO_REQUEST;
    }
    if (payload_len > CHIPS_MAX_PAYLOAD || (payload == NULL && payload_len > 0)) {
        return CHIPS_RESPOND_INVALID_ARG;
    }

    tx_abort(s);

    s->tx_len = chips_encode(s->pending.seq, CHIPS_DIR_RESPONSE, s->pending.cmd, payload, payload_len, s->tx_frame);
    CHIPS_ASSERT(s->tx_len > 0 && s->tx_len <= sizeof(s->tx_frame));
    s->tx_off = 0;

    s->cache_valid = true;
    s->cached = s->pending;
    s->t_cached = s->port.now_us(s->port.ctx);

    release_request(s);
    return CHIPS_RESPOND_OK;
}

chips_respond_status_t chips_slave_abandon(chips_slave_t* s) {
    CHIPS_ASSERT(s != NULL);

    if (s->state != CHIPS_SLAVE_PENDING) {
        return CHIPS_RESPOND_NO_REQUEST;
    }
    release_request(s);
    return CHIPS_RESPOND_OK;
}

void chips_slave_invalidate_cache(chips_slave_t* s) {
    CHIPS_ASSERT(s != NULL);
    tx_abort(s);
    s->cache_valid = false;
}

chips_slave_state_t chips_slave_state(const chips_slave_t* s) {
    CHIPS_ASSERT(s != NULL);
    return s->state;
}
