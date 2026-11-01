/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 EPFL Spacecraft Team
 */

#include "chips/link.h"

void chips_link_init(chips_link_t* l, const chips_port_t* port) {
    CHIPS_ASSERT(l != NULL && port != NULL);
    CHIPS_ASSERT(port->write != NULL && port->read != NULL && port->now != NULL);

    *l = (chips_link_t){0};
    l->port = *port;
    chips_decoder_init(&l->dec, NULL);
}

bool chips_link_encode(chips_link_t* l,
                       uint8_t seq,
                       chips_direction_t direction,
                       uint8_t cmd,
                       const uint8_t* payload,
                       size_t payload_len) {
    const size_t len = chips_encode(seq, direction, cmd, payload, payload_len, l->tx_frame);
    if (len == 0u) {
        return false;
    }
    l->tx_len = len;
    l->tx_off = len;
    return true;
}

void chips_link_restart(chips_link_t* l) {
    l->tx_off = 0;
}

void chips_link_abort(chips_link_t* l) {
    l->tx_off = l->tx_len;
}

bool chips_link_tx_busy(const chips_link_t* l) {
    return l->tx_off < l->tx_len;
}

chips_link_tx_t chips_link_write(chips_link_t* l) {
    if (!chips_link_tx_busy(l)) {
        return CHIPS_LINK_TX_NONE;
    }

    const size_t want = l->tx_len - l->tx_off;
    size_t written = 0;
    if (l->port.write(l->port.ctx, &l->tx_frame[l->tx_off], want, &written) != CHIPS_IO_OK) {
        chips_link_abort(l);
        return CHIPS_LINK_TX_ERROR;
    }
    CHIPS_ASSERT(written <= want);
    l->tx_off += (written < want) ? written : want;

    return chips_link_tx_busy(l) ? CHIPS_LINK_TX_NONE : CHIPS_LINK_TX_DONE;
}

bool chips_link_read(chips_link_t* l) {
    size_t n = 0;
    l->rx_pos = 0;
    l->rx_len = 0;
    if (l->port.read(l->port.ctx, l->rx_chunk, sizeof(l->rx_chunk), &n) != CHIPS_IO_OK) {
        (void)chips_decoder_reset(&l->dec);
        return false;
    }
    CHIPS_ASSERT(n <= sizeof(l->rx_chunk));
    l->rx_len = (n < sizeof(l->rx_chunk)) ? n : sizeof(l->rx_chunk);
    return true;
}

chips_dec_status_t chips_link_next(chips_link_t* l) {
    size_t used = 0;
    const chips_dec_status_t st =
        chips_decode(&l->dec, &l->rx_chunk[l->rx_pos], l->rx_len - l->rx_pos, &used, &l->frame);
    CHIPS_ASSERT(st != CHIPS_DEC_ERR_ARG);
    l->rx_pos += used;
    return st;
}

size_t chips_link_unread(const chips_link_t* l) {
    return l->rx_len - l->rx_pos;
}
