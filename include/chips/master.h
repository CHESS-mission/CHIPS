/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 EPFL Spacecraft Team
 */

#ifndef CHIPS_MASTER_H
#define CHIPS_MASTER_H

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
 *
 * Sets the receive throughput ceiling: CHIPS_MASTER_RX_CHUNK bytes per tick.
 * May be overridden at compile time.
 */
#ifndef CHIPS_MASTER_RX_CHUNK
#define CHIPS_MASTER_RX_CHUNK (256u)
#endif

/** @brief Static configuration. */
typedef struct {
    /** Delay between the starts of two copies of a request, in µs. */
    uint32_t resend_period_us;
} chips_master_config_t;

/* ------------------------------------------------------------------------- */
/* Results and events                                                        */
/* ------------------------------------------------------------------------- */

/** @brief Result of chips_master_submit(). */
typedef enum {
    CHIPS_SUBMIT_OK = 0,     /**< Request accepted. */
    CHIPS_SUBMIT_BUSY,       /**< A request is already in flight. */
    CHIPS_SUBMIT_INVALID_ARG /**< Bad command, payload, response buffer or timeout. */
} chips_submit_status_t;

/**
 * @brief How a transaction ended.
 *
 * Only a response frame whose sequence and command both match the request
 * ends a transaction early. Every other decoded frame is ignored and reported
 * through an event bit (#CHIPS_EV_RX_REQUEST, #CHIPS_EV_RX_STALE,
 * #CHIPS_EV_RX_CMD_MISMATCH), and the master keeps listening until the
 * deadline.
 */
typedef enum {
    CHIPS_OUTCOME_PENDING = 0, /**< Not finished (or none submitted). */
    CHIPS_OUTCOME_OK,          /**< Response with matching sequence and command received. */
    CHIPS_OUTCOME_TIMEOUT,     /**< Deadline passed after at least one full send. */
    CHIPS_OUTCOME_SEND_FAILED, /**< Deadline passed and no copy was ever fully sent. */
    CHIPS_OUTCOME_CANCELLED    /**< chips_master_cancel() was called. */
} chips_outcome_t;

/** @brief Event bits returned by chips_master_step(). */
enum {
    CHIPS_EV_NONE = 0u,
    CHIPS_EV_DONE = 1u << 0,           /**< Transaction finished; see chips_master_result(). */
    CHIPS_EV_TX_FRAME = 1u << 1,       /**< A full copy of the request reached the port. */
    CHIPS_EV_TX_ERROR = 1u << 2,       /**< Port write failed; copy abandoned until next resend. */
    CHIPS_EV_RX_ERROR = 1u << 3,       /**< Port read failed; partial frame discarded. */
    CHIPS_EV_RX_BAD_FRAME = 1u << 4,   /**< Decoder rejected a frame (escape, length, CRC). */
    CHIPS_EV_RX_STALE = 1u << 5,       /**< Response with another sequence (or before first send) ignored. */
    CHIPS_EV_RX_DROPPED = 1u << 6,     /**< Bytes read but not decoded (idle or after completion). */
    CHIPS_EV_RX_REQUEST = 1u << 7,     /**< Frame flagged as a request ignored (e.g. line echo). */
    CHIPS_EV_RX_CMD_MISMATCH = 1u << 8 /**< Response with matching sequence but other command ignored. */
};

/** @brief Outcome of the last transaction. */
typedef struct {
    chips_outcome_t outcome;
    uint8_t seq; /**< Sequence number used by the request. */
    uint8_t cmd; /**< Command of the request. */
    /**
     * Response payload when OK, else NULL. Equal to the response buffer given
     * to the chips_master_submit() that started this transaction; it is not a
     * separate copy. The pointer is cleared by the next chips_master_submit();
     * the bytes stay in the caller's buffer, which the master no longer touches
     * once the transaction has ended.
     */
    const uint8_t* payload;
    size_t payload_len;  /**< Response payload length when OK, else 0. */
    uint32_t latency_us; /**< First step to completion; 0 if cancelled before the first step. */
} chips_master_result_t;

/* ------------------------------------------------------------------------- */
/* Instance                                                                  */
/* ------------------------------------------------------------------------- */

/** @brief Master state. */
typedef enum {
    CHIPS_MASTER_IDLE = 0, /**< No request in flight. */
    CHIPS_MASTER_ARMED,    /**< Submitted; waiting for the first step. */
    CHIPS_MASTER_WAITING   /**< Sending and waiting for the response. */
} chips_master_state_t;

/**
 * @brief Master instance.
 *
 * Fields are visible only so the instance can be allocated statically.
 * Treat as opaque and use the functions below.
 *
 * The instance owns its transmit frame and receive chunk. It does not own
 * any response buffer: each chips_master_submit() lends one for the duration
 * of that transaction only (see chips_master_submit()).
 */
typedef struct {
    /* Binding */
    chips_port_t port;
    uint32_t resend_period_us;

    /* Transaction */
    chips_master_state_t state;
    uint8_t seq;
    uint8_t cmd;
    uint32_t timeout_us;
    uint32_t t_start;     /**< Time of the first step. */
    uint32_t t_last_send; /**< Start time of the latest copy. */
    bool tx_started;      /**< At least one copy was started. */
    bool sent_ok;         /**< At least one copy was fully written. */

    /* Tx: a copy is in flight while tx_off < tx_len. */
    size_t tx_len;
    size_t tx_off;
    uint8_t tx_frame[CHIPS_WIRE_MAX];

    /* Rx: the decoder writes into the response buffer of the current submit. */
    chips_decoder_t dec;
    chips_frame_t frame;
    uint8_t rx_chunk[CHIPS_MASTER_RX_CHUNK];

    /* Output */
    chips_master_result_t result;
} chips_master_t;

/* ------------------------------------------------------------------------- */
/* API                                                                       */
/* ------------------------------------------------------------------------- */

/**
 * @brief Initialize an instance.
 *
 * Clears all state. May be called again to re-initialize; a request in
 * flight is then abandoned without I/O and its response buffer released.
 *
 * @param m    Instance.
 * @param port Port; copied. All three functions must be non-NULL.
 * @param cfg  Configuration; resend_period_us in 1..#CHIPS_TIME_MAX_US.
 */
void chips_master_init(chips_master_t* m, const chips_port_t* port, const chips_master_config_t* cfg);

/**
 * @brief Queue a request. No I/O happens until the next step.
 *
 * Clears the previous result (including its payload pointer).
 *
 * Ownership and lifetime of @p response_buf:
 *  - Borrowed, not copied, for this transaction only. The master never frees
 *    it.
 *  - Lent from this call until the transaction ends: chips_master_step()
 *    returns #CHIPS_EV_DONE, chips_master_cancel() returns true, or the
 *    instance is re-initialized. In other words, while chips_master_state()
 *    is not #CHIPS_MASTER_IDLE. During that time the buffer must stay valid,
 *    the application must not write to it, and it must not be given to
 *    another master, slave or decoder.
 *  - The master writes to it only inside chips_master_step(). Nothing is
 *    written by this call, by chips_master_cancel(), or after the transaction
 *    has ended; the next transaction uses only its own buffer.
 *  - It also holds partially received and ignored frames. Its contents are
 *    therefore meaningful only after the transaction ended with
 *    #CHIPS_OUTCOME_OK (the first payload_len bytes); after any other outcome
 *    they are unspecified.
 *  - Once the transaction has ended, the buffer is the application's again.
 *
 * @param m            Instance.
 * @param cmd          Command; at most #CHIPS_CMD_MASK.
 * @param payload      Request payload; encoded (copied) before this call
 *                     returns, so it may be released or reused immediately,
 *                     and may point into @p response_buf. May be NULL if
 *                     @p payload_len is 0.
 * @param payload_len  At most #CHIPS_MAX_PAYLOAD.
 * @param timeout_us   1..#CHIPS_TIME_MAX_US, measured from the first step.
 * @param response_buf Where the response payload is written, #CHIPS_MAX_PAYLOAD
 *                     bytes; non-NULL. Lent as described above.
 */
chips_submit_status_t chips_master_submit(chips_master_t* m,
                                          uint8_t cmd,
                                          const uint8_t* payload,
                                          size_t payload_len,
                                          uint32_t timeout_us,
                                          uint8_t response_buf[CHIPS_MAX_PAYLOAD]);

/**
 * @brief Advance the state machine by one tick.
 *
 * @return Bitwise OR of CHIPS_EV_* for everything that happened in this step.
 */
uint32_t chips_master_step(chips_master_t* m);

/**
 * @brief Abandon the request in flight, if any. Does no I/O.
 *
 * A copy being written is cut short; the slave discards the partial frame.
 * The response buffer is released immediately.
 *
 * @return true if a request was cancelled.
 */
bool chips_master_cancel(chips_master_t* m);

/** @brief Current state. */
chips_master_state_t chips_master_state(const chips_master_t* m);

/** @brief Result of the latest transaction (PENDING while one is in flight). */
const chips_master_result_t* chips_master_result(const chips_master_t* m);

#ifdef __cplusplus
}
#endif

#endif /* CHIPS_MASTER_H */
