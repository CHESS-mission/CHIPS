/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 EPFL Spacecraft Team
 */

#ifndef CHIPS_SLAVE_H
#define CHIPS_SLAVE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "chips/core.h"
#include "chips/port.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------- */
/* Configuration                                                             */
/* ------------------------------------------------------------------------- */

/**
 * @brief Maximum bytes read from the port per step.
 */
#ifndef CHIPS_SLAVE_RX_CHUNK
#define CHIPS_SLAVE_RX_CHUNK (256u)
#endif

/** @brief Static configuration. */
typedef struct {
    /**
     * How long a cached response may be replayed, in µs, measured from the
     * chips_slave_respond() call. 1..#CHIPS_TIME_MAX_US. Should be at least
     * the master's longest transaction timeout.
     */
    uint32_t cache_window_us;
} chips_slave_config_t;

/* ------------------------------------------------------------------------- */
/* Results and events                                                        */
/* ------------------------------------------------------------------------- */

/** @brief Result of chips_slave_respond() and chips_slave_abandon(). */
typedef enum {
    CHIPS_RESPOND_OK = 0,     /**< Response queued (or request abandoned). */
    CHIPS_RESPOND_NO_REQUEST, /**< No request is pending. */
    CHIPS_RESPOND_INVALID_ARG /**< Payload too long, or NULL with a nonzero length. */
} chips_respond_status_t;

/** @brief Event bits returned by chips_slave_step(). */
enum {
    CHIPS_SLAVE_EV_NONE = 0u,
    CHIPS_SLAVE_EV_REQUEST = 1u << 0,      /**< New request pending; see chips_slave_request(). */
    CHIPS_SLAVE_EV_TX_FRAME = 1u << 1,     /**< A full copy of a response reached the port. */
    CHIPS_SLAVE_EV_RETRANSMIT = 1u << 2,   /**< Duplicate request answered from the cache. */
    CHIPS_SLAVE_EV_TX_ERROR = 1u << 3,     /**< Port write failed; that copy is abandoned. */
    CHIPS_SLAVE_EV_RX_ERROR = 1u << 4,     /**< Port read failed; partial frame discarded. */
    CHIPS_SLAVE_EV_RX_BAD_FRAME = 1u << 5, /**< Decoder rejected a frame (escape, length, CRC). */
    CHIPS_SLAVE_EV_RX_IGNORED = 1u << 6    /**< Valid frame not acted on (busy, pending copy, response). */
};

/**
 * @brief The request the application must answer.
 *
 * The payload is not copied: it points into the half of the slave's receive
 * double buffer that the request was decoded into. That half is withdrawn
 * from the decoder when the request is delivered, so later traffic cannot
 * overwrite it. The payload stays valid and unchanged until
 * chips_slave_respond() or chips_slave_abandon(), and may be passed back to
 * chips_slave_respond() (e.g. to echo it).
 */
typedef struct {
    uint8_t seq;
    uint8_t cmd;
    const uint8_t* payload;
    size_t payload_len;
} chips_slave_request_t;

/* ------------------------------------------------------------------------- */
/* Instance                                                                  */
/* ------------------------------------------------------------------------- */

/** @brief Slave state. */
typedef enum {
    CHIPS_SLAVE_LISTENING = 0, /**< Waiting for a request. */
    CHIPS_SLAVE_PENDING        /**< The application owns a request. */
} chips_slave_state_t;

/** @brief Identity of a request, used to recognise duplicates. */
typedef struct {
    uint8_t seq;
    uint8_t cmd;
    uint16_t crc;
} chips_slave_fingerprint_t;

/**
 * @brief Slave instance.
 *
 * Treat as opaque; access only through the functions below.
 */
typedef struct {
    /* Binding */
    chips_port_t port;
    uint32_t cache_window_us;

    /* State */
    chips_slave_state_t state;
    uint32_t now; /**< Time sampled by the latest step. */

    /* Pending request (valid in PENDING); its payload lives in rx_payload. */
    chips_slave_fingerprint_t pending;
    chips_slave_request_t request;

    /* Response cache: a copy is in flight while tx_off < tx_len. */
    bool cache_valid;
    chips_slave_fingerprint_t cached;
    uint32_t t_cached; /**< Time when the response was queued. */
    size_t tx_len;
    size_t tx_off;
    uint8_t tx_frame[CHIPS_WIRE_MAX];

    /*
     * Rx. Double buffer: the decoder writes into rx_payload[rx_active]. On
     * delivery, that half becomes the pending request's payload and the
     * decoder moves to the other half.
     */
    chips_decoder_t dec;
    chips_frame_t frame;
    uint8_t rx_chunk[CHIPS_SLAVE_RX_CHUNK];
    uint8_t rx_payload[2][CHIPS_MAX_PAYLOAD];
    uint8_t rx_active;
} chips_slave_t;

/* ------------------------------------------------------------------------- */
/* API                                                                       */
/* ------------------------------------------------------------------------- */

/**
 * @brief Initialize an instance. Clears all state and the cache.
 *
 * @param s    Instance.
 * @param port Port; copied. All three functions must be non-NULL.
 * @param cfg  Configuration.
 */
void chips_slave_init(chips_slave_t* s, const chips_port_t* port, const chips_slave_config_t* cfg);

/**
 * @brief Advance the state machine by one tick.
 *
 * Order within a step: receive and decode (delivering at most one new
 * request, answering duplicates), then transmit.
 *
 * @return Bitwise OR of CHIPS_SLAVE_EV_* for everything that happened in this step.
 */
uint32_t chips_slave_step(chips_slave_t* s);

/**
 * @brief The pending request, or NULL when listening.
 */
const chips_slave_request_t* chips_slave_request(const chips_slave_t* s);

/**
 * @brief Answer the pending request. Does no I/O.
 *
 * Encodes the response, replaces the cache with it (cutting short any copy
 * of the previous response still being written), and returns to LISTENING.
 * The first copy is written by the next step.
 *
 * @param s           Instance.
 * @param payload     Response payload; copied. May be NULL if @p payload_len
 *                    is 0. May point into the pending request's payload.
 * @param payload_len At most #CHIPS_MAX_PAYLOAD.
 */
chips_respond_status_t chips_slave_respond(chips_slave_t* s, const uint8_t* payload, size_t payload_len);

/**
 * @brief Decline the pending request: send nothing and return to LISTENING.
 *
 * Nothing is cached, so a resend of the same request is delivered again as
 * new. The existing cache (for an earlier request) is left untouched.
 */
chips_respond_status_t chips_slave_abandon(chips_slave_t* s);

/**
 * @brief Drop the response cache, so no duplicate is answered from it.
 *
 * Use after a mode change or reset that makes the cached answer wrong.
 * A copy being written is cut short. Does not affect a pending request.
 */
void chips_slave_invalidate_cache(chips_slave_t* s);

/** @brief Current state. */
chips_slave_state_t chips_slave_state(const chips_slave_t* s);

#ifdef __cplusplus
}
#endif

#endif
