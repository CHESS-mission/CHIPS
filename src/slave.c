/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 EPFL Spacecraft Team
 */

#include "chips/slave.h"

static bool same_request(const chips_slave_fingerprint_t* fp, const chips_frame_t* f) {
    return fp->seq == f->seq && fp->cmd == f->cmd && fp->crc == f->crc;
}

static void expire_cache(chips_slave_t* s, chips_time_t now) {
    if (s->cache_valid && chips_time_cmp(chips_time_sub(now, s->t_cached), s->cache_window) >= 0) {
        s->cache_valid = false;
    }
}

static uint32_t deliver(chips_slave_t* s) {
    const chips_frame_t* f = chips_link_frame(&s->link);
    CHIPS_ASSERT(f->payload == s->rx_payload[s->rx_active]);

    s->request.seq = f->seq;
    s->request.cmd = f->cmd;
    s->request.payload = f->payload;
    s->request.payload_len = f->payload_len;
    s->request_crc = f->crc;

    s->rx_active ^= 1u;
    chips_link_rx_swap(&s->link, s->rx_payload[s->rx_active]);

    s->state = CHIPS_SLAVE_PENDING;
    return CHIPS_SLAVE_EV_REQUEST;
}

static uint32_t on_frame(chips_slave_t* s) {
    const chips_frame_t* f = chips_link_frame(&s->link);

    if (f->direction != CHIPS_DIR_REQUEST || s->state == CHIPS_SLAVE_PENDING) {
        return CHIPS_SLAVE_EV_RX_IGNORED;
    }
    if (s->cache_valid && same_request(&s->cached, f)) {
        if (chips_link_tx_busy(&s->link)) {
            return CHIPS_SLAVE_EV_RX_IGNORED;
        }
        chips_link_restart(&s->link);
        return CHIPS_SLAVE_EV_RETRANSMIT;
    }
    return deliver(s);
}

static uint32_t receive(chips_slave_t* s) {
    uint32_t ev = CHIPS_SLAVE_EV_NONE;

    if (!chips_link_read(&s->link)) {
        return CHIPS_SLAVE_EV_RX_ERROR;
    }
    for (;;) {
        const chips_dec_status_t st = chips_link_next(&s->link);
        if (st == CHIPS_DEC_NEED_MORE) {
            return ev;
        }
        ev |= (st == CHIPS_DEC_FRAME) ? on_frame(s) : CHIPS_SLAVE_EV_RX_BAD_FRAME;
    }
}

static void release_request(chips_slave_t* s) {
    s->request = (chips_slave_request_t){0};
    s->state = CHIPS_SLAVE_LISTENING;
}

void chips_slave_init(chips_slave_t* s, const chips_port_t* port, const chips_timing_t* timing) {
    CHIPS_ASSERT(s != NULL);

    *s = (chips_slave_t){0};
    chips_link_init(&s->link, port);
    chips_link_rx_attach(&s->link, s->rx_payload[s->rx_active]);
    s->cache_window = chips_timing_cache_window(timing);
}

uint32_t chips_slave_step(chips_slave_t* s) {
    CHIPS_ASSERT(s != NULL);

    expire_cache(s, chips_link_now(&s->link));

    const uint32_t ev = receive(s);
    return ev | chips_link_write(&s->link);
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
    if (!chips_link_encode(&s->link, s->request.seq, CHIPS_DIR_RESPONSE, s->request.cmd, payload, payload_len)) {
        return CHIPS_RESPOND_INVALID_ARG;
    }
    chips_link_restart(&s->link);

    s->cache_valid = true;
    s->cached.seq = s->request.seq;
    s->cached.cmd = s->request.cmd;
    s->cached.crc = s->request_crc;
    s->t_cached = chips_link_now(&s->link);

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
    chips_link_abort(&s->link);
    s->cache_valid = false;
}

chips_slave_state_t chips_slave_state(const chips_slave_t* s) {
    CHIPS_ASSERT(s != NULL);
    return s->state;
}
