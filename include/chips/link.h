/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 EPFL Spacecraft Team
 */

/**
 * @file link.h
 * @brief Framed byte-stream I/O shared by the master and the slave. @internal
 *
 * A link owns one outgoing frame, written to the port a piece at a time, and
 * one receive chunk, decoded frame by frame. It knows nothing about requests,
 * responses or timing; the master and slave build their policies on top of it.
 *
 * Declared publicly only so that chips_master_t and chips_slave_t can embed
 * it. Applications should not call these functions.
 */

#ifndef CHIPS_LINK_H
#define CHIPS_LINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "chips/core.h"
#include "chips/port.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Event bits shared by the master and slave event sets.
 *
 * Bit 0 is left to the master and slave for their own completion event.
 */
enum {
    CHIPS_LINK_EV_TX_FRAME = 1u << 1,    /**< The last byte of a copy reached the port. */
    CHIPS_LINK_EV_TX_ERROR = 1u << 2,    /**< Port write failed; the copy is abandoned. */
    CHIPS_LINK_EV_RX_ERROR = 1u << 3,    /**< Port read failed; partial frame discarded. */
    CHIPS_LINK_EV_RX_BAD_FRAME = 1u << 4 /**< Decoder rejected a frame (escape, length, CRC). */
};

/** @brief Link state. Treat as opaque. */
typedef struct {
    chips_port_t port;

    /* Tx: a copy is in flight while tx_off < tx_len. */
    size_t tx_len;
    size_t tx_off;
    uint8_t tx_frame[CHIPS_WIRE_MAX];

    /* Rx: rx_chunk[rx_pos..rx_len) is read but not decoded yet. */
    chips_decoder_t dec;
    chips_frame_t frame; /**< Last frame returned by chips_link_next(). */
    size_t rx_pos;
    size_t rx_len;
    uint8_t rx_chunk[CHIPS_RX_CHUNK];
} chips_link_t;

/** @brief Clear all state and bind @p port. The decoder has no buffer yet. */
void chips_link_init(chips_link_t* l, const chips_port_t* port);

/** @brief Read the port's clock. */
chips_time_t chips_link_now(const chips_link_t* l);

/**
 * @brief Decode into @p payload_buf from now on, discarding any partially
 *        received frame.
 */
void chips_link_rx_attach(chips_link_t* l, uint8_t payload_buf[CHIPS_MAX_PAYLOAD]);

/**
 * @brief Decode into @p payload_buf from now on, keeping the framing state.
 *
 * For use right after chips_link_next() returned #CHIPS_DEC_FRAME, so the
 * frame's payload stays in the old buffer; see chips_decoder_set_buffer().
 */
void chips_link_rx_swap(chips_link_t* l, uint8_t payload_buf[CHIPS_MAX_PAYLOAD]);

/**
 * @brief Replace the outgoing frame, cutting short any copy in flight.
 *
 * The new frame is not started; see chips_link_restart().
 *
 * @return false, leaving the link untouched, if chips_encode() rejects the
 *         arguments.
 */
bool chips_link_encode(chips_link_t* l,
                       uint8_t seq,
                       chips_direction_t direction,
                       uint8_t cmd,
                       const uint8_t* payload,
                       size_t payload_len);

/** @brief Start a new copy of the outgoing frame from its first byte. */
void chips_link_restart(chips_link_t* l);

/** @brief Stop writing the current copy. Bytes already written stay on the line. */
void chips_link_abort(chips_link_t* l);

/** @brief true while a copy is being written. */
bool chips_link_tx_busy(const chips_link_t* l);

/**
 * @brief Write as much of the current copy as the port accepts.
 *
 * @return #CHIPS_LINK_EV_TX_FRAME when the copy is complete,
 *         #CHIPS_LINK_EV_TX_ERROR on a port error, else 0.
 */
uint32_t chips_link_write(chips_link_t* l);

/**
 * @brief Read one chunk from the port, discarding any undecoded bytes of the
 *        previous one.
 *
 * @return false on a port error; the partially received frame is discarded.
 */
bool chips_link_read(chips_link_t* l);

/**
 * @brief Decode the next frame from the current chunk; see chips_link_frame().
 *
 * @return #CHIPS_DEC_FRAME, an error status for a rejected frame, or
 *         #CHIPS_DEC_NEED_MORE once the chunk is used up.
 */
chips_dec_status_t chips_link_next(chips_link_t* l);

/** @brief The frame decoded by the last chips_link_next() that returned #CHIPS_DEC_FRAME. */
const chips_frame_t* chips_link_frame(const chips_link_t* l);

/** @brief Bytes of the current chunk not decoded yet. */
size_t chips_link_unread(const chips_link_t* l);

#ifdef __cplusplus
}
#endif

#endif /* CHIPS_LINK_H */
