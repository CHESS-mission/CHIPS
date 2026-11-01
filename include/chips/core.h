/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 EPFL Spacecraft Team
 */

/**
 * @file core.h
 * @brief CHIPS protocol core: framing, byte stuffing, CRC-16/Kermit and timing.
 *
 * Provides the building blocks for exchanging CHIPS packets over a byte
 * stream:
 *  - a table-driven CRC-16/Kermit implementation,
 *  - an encoder producing delimited, byte-stuffed wire frames,
 *  - a streaming decoder tolerant of arbitrary input fragmentation,
 *  - wrapping-safe time arithmetic and the retransmission timing shared by
 *    master and slave.
 *
 * Wire format (before byte stuffing, between #CHIPS_SYNC delimiters):
 * @verbatim
 *   +--------+-------------+-------------------+-----------+
 *   |  seq   | dir | cmd   | payload (0..1024) | CRC16     |
 *   | 1 byte | 1 byte      | N bytes           | 2 bytes   |
 *   +--------+-------------+-------------------+-----------+
 * @endverbatim
 *
 * Any #CHIPS_SYNC or #CHIPS_ESC byte inside a frame is transmitted as
 * #CHIPS_ESC followed by the corresponding substitute byte.
 */

#ifndef CHIPS_CORE_H
#define CHIPS_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef CHIPS_CONFIG_FILE
#include CHIPS_CONFIG_FILE
#endif

/*
 * Contract checks. Functions that report invalid input through their result
 * (chips_encode(), chips_decode(), chips_master_submit(), ...) check it at run
 * time. Every other precondition is checked only by CHIPS_ASSERT; with
 * assertions disabled, violating one is undefined behaviour.
 */
#ifndef CHIPS_ASSERT
#include <assert.h>
#define CHIPS_ASSERT(expr) (assert(expr))
#endif

#ifndef CHIPS_NO_LIBC_STRING
#include <string.h>  // IWYU pragma: keep
#endif

#ifndef CHIPS_CRC16_KERMIT_FOLD
#define CHIPS_CRC16_KERMIT_FOLD (1u)
#endif

#ifndef CHIPS_RX_CHUNK
#define CHIPS_RX_CHUNK (256u)
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------- */
/* Protocol constants                                                        */
/* ------------------------------------------------------------------------- */

#define CHIPS_CRC16_KERMIT_INITIAL (0x0000u) /**< Initial CRC value. */

#define CHIPS_SYNC (0x7Eu)     /**< Frame delimiter. */
#define CHIPS_ESC (0x7Du)      /**< Escape byte. */
#define CHIPS_SYNC_SUB (0x5Eu) /**< Substitute sent after ESC for SYNC. */
#define CHIPS_ESC_SUB (0x5Du)  /**< Substitute sent after ESC for ESC. */

#define CHIPS_CMD_MASK (0x7Fu)  /**< Command identifier bits. */
#define CHIPS_FLAG_RESP (0x80u) /**< Response flag in the command byte. */

#define CHIPS_MAX_PAYLOAD (1024u) /**< Maximum payload size in bytes. */
#define CHIPS_HEADER_SIZE (2u)    /**< Header size in bytes. */
#define CHIPS_CRC_SIZE (2u)       /**< CRC size in bytes. */

/** Smallest unstuffed frame (header + CRC, empty payload). */
#define CHIPS_FRAME_MIN (CHIPS_HEADER_SIZE + CHIPS_CRC_SIZE)

/** Largest unstuffed frame (header + maximum payload + CRC). */
#define CHIPS_FRAME_MAX (CHIPS_HEADER_SIZE + CHIPS_MAX_PAYLOAD + CHIPS_CRC_SIZE)

/**
 * @brief Worst-case encoded size for a payload of @p payload_len bytes.
 *
 * Includes both delimiters and assumes every frame byte is escaped.
 */
#define CHIPS_WIRE_SIZE(payload_len) (2u + 2u * (CHIPS_FRAME_MIN + (payload_len)))

/** Worst-case encoded size of any frame. */
#define CHIPS_WIRE_MAX CHIPS_WIRE_SIZE(CHIPS_MAX_PAYLOAD)

/** Nanoseconds per second; every valid #chips_time_t has tv_nsec below this. */
#define CHIPS_NSEC_PER_SEC (1000000000u)

/** Longest duration accepted anywhere in the API, in whole seconds (about 68 years). */
#define CHIPS_TIME_MAX_SEC (0x7FFFFFFFu)

/* ------------------------------------------------------------------------- */
/* CRC-16/Kermit                                                             */
/* ------------------------------------------------------------------------- */

/**
 * @brief Compute or update a CRC-16/Kermit checksum.
 *
 * Pass #CHIPS_CRC16_KERMIT_INITIAL to start a new checksum, or a previous
 * result to continue over additional data.
 *
 * @param data   Input bytes; may be NULL if @p length is 0.
 * @param length Number of bytes in @p data.
 * @param crc    Starting CRC value.
 *
 * @return Updated CRC.
 */
uint16_t chips_crc16_kermit_compute(const uint8_t* data, size_t length, uint16_t crc);

/* ------------------------------------------------------------------------- */
/* Encoder                                                                   */
/* ------------------------------------------------------------------------- */

/** @brief Frame direction. */
typedef enum {
    CHIPS_DIR_REQUEST, /**< Request. */
    CHIPS_DIR_RESPONSE /**< Response to a previous request. */
} chips_direction_t;

/**
 * @brief Encode a packet into wire format.
 *
 * Produces a complete frame: delimiters, header, stuffed payload and CRC.
 *
 * @param seq         Sequence number.
 * @param direction   Request or response.
 * @param cmd         Command identifier; must fit in #CHIPS_CMD_MASK.
 * @param payload     Payload bytes; may be NULL if @p payload_len is 0.
 * @param payload_len Payload size; must not exceed #CHIPS_MAX_PAYLOAD.
 * @param out         Destination buffer of at least #CHIPS_WIRE_MAX bytes.
 *
 * @return Number of bytes written to @p out.
 */
size_t chips_encode(uint8_t seq,
                    chips_direction_t direction,
                    uint8_t cmd,
                    const uint8_t* payload,
                    size_t payload_len,
                    uint8_t out[CHIPS_WIRE_MAX]);

/* ------------------------------------------------------------------------- */
/* Decoder                                                                   */
/* ------------------------------------------------------------------------- */

/** @brief Status returned by chips_decode(). */
typedef enum {
    CHIPS_DEC_NEED_MORE = 0, /**< Input exhausted; no complete frame yet. */
    CHIPS_DEC_FRAME,         /**< A valid frame was decoded. */
    CHIPS_DEC_ERR_ESCAPE,    /**< Invalid escape sequence. */
    CHIPS_DEC_ERR_TOO_LONG,  /**< Frame exceeds #CHIPS_FRAME_MAX bytes. */
    CHIPS_DEC_ERR_TOO_SHORT, /**< Frame shorter than #CHIPS_FRAME_MIN bytes. */
    CHIPS_DEC_ERR_CRC,       /**< CRC mismatch. */
    CHIPS_DEC_ERR_ARG        /**< Invalid argument. */
} chips_dec_status_t;

/**
 * @brief A decoded frame.
 *
 * @p payload points into the decoder's buffer and is valid until the next
 * call to chips_decode() or chips_decoder_reset() on the same decoder.
 */
typedef struct {
    uint8_t seq;                 /**< Sequence number. */
    chips_direction_t direction; /**< Request or response. */
    uint8_t cmd;                 /**< Command identifier. */
    const uint8_t* payload;      /**< Payload data. */
    size_t payload_len;          /**< Payload length in bytes. */
    uint16_t crc;                /**< CRC-16/Kermit received with the frame. */
} chips_frame_t;

/** @brief Decoder receive state. @internal */
typedef enum {
    CHIPS_RX_HUNT = 0, /**< Waiting for a frame delimiter. */
    CHIPS_RX_DATA,     /**< Receiving frame bytes. */
    CHIPS_RX_ESCAPED   /**< Previous byte was #CHIPS_ESC. */
} chips_rx_state_t;

/**
 * @brief Streaming decoder instance.
 *
 * Treat as opaque; access only through the functions below.
 */
typedef struct {
    uint8_t* payload;               /**< User-supplied payload buffer. */
    size_t len;                     /**< Bytes received in current frame. */
    uint8_t hdr[CHIPS_HEADER_SIZE]; /**< Received header bytes. */
    uint8_t spill[CHIPS_CRC_SIZE];  /**< Frame bytes past header + #CHIPS_MAX_PAYLOAD: CRC of a full-size frame. */
    chips_rx_state_t state;         /**< Receive state. */
} chips_decoder_t;

/**
 * @brief Initialize a decoder.
 *
 * @param dec         Decoder to initialize.
 * @param payload_buf Payload storage of #CHIPS_MAX_PAYLOAD bytes. Must
 *                    outlive the decoder.
 */
void chips_decoder_init(chips_decoder_t* dec, uint8_t payload_buf[CHIPS_MAX_PAYLOAD]);

/**
 * @brief Discard any partially received frame.
 *
 * The payload buffer binding is kept.
 *
 * @param dec Decoder to reset.
 *
 * @return Number of bytes discarded (header, payload and CRC).
 */
size_t chips_decoder_reset(chips_decoder_t* dec);

/**
 * @brief Point the decoder at a different payload buffer, keeping framing state.
 *
 * Unlike chips_decoder_init(), this does not return to the hunt state, so a
 * frame that starts on the delimiter which closed the previous one is not
 * lost. Intended for rotating buffers: once chips_decode() has returned
 * #CHIPS_DEC_FRAME, the frame's payload can be kept in the old buffer while
 * decoding continues into the new one.
 *
 * Precondition: no payload byte of the current frame is stored yet (always
 * true right after chips_decode() returned #CHIPS_DEC_FRAME, or after init or
 * reset).
 *
 * @param dec         Decoder.
 * @param payload_buf New payload storage of #CHIPS_MAX_PAYLOAD bytes. Must
 *                    outlive its use by the decoder.
 */
void chips_decoder_set_buffer(chips_decoder_t* dec, uint8_t payload_buf[CHIPS_MAX_PAYLOAD]);

/**
 * @brief Feed bytes from a CHIPS stream into the decoder.
 *
 * Input may contain partial frames, several frames, or arbitrary
 * fragmentation. Decoding stops once a frame or an error is reported;
 * call again with the remaining input (@p in + @p *consumed) to continue.
 *
 * Progress guarantee: if @p in_len > 0 and the return value is not
 * #CHIPS_DEC_ERR_ARG, then 1 <= @p *consumed <= @p in_len. Every such
 * call consumes at least one byte, so a caller loop that feeds the
 * remaining input back in terminates after at most @p in_len calls.
 *
 * @param dec      Decoder instance.
 * @param in       Input bytes.
 * @param in_len   Number of input bytes.
 * @param consumed Receives the number of bytes consumed from @p in.
 * @param frame    Receives the decoded frame when #CHIPS_DEC_FRAME is
 *                 returned; unspecified otherwise.
 *
 * @return A #chips_dec_status_t value.
 */
chips_dec_status_t chips_decode(chips_decoder_t* dec,
                                const uint8_t* in,
                                size_t in_len,
                                size_t* consumed,
                                chips_frame_t* frame);

/* ------------------------------------------------------------------------- */
/* Time                                                                      */
/* ------------------------------------------------------------------------- */

/**
 * @brief A time in seconds and nanoseconds, like POSIX struct timespec.
 *
 * Used both for clock readings (chips_port_t::now) and for durations. A
 * clock reading's tv_sec may wrap at 2^32; durations between two readings
 * are taken modulo 2^32 s, so the wrap is harmless. Durations passed to the
 * API must not exceed #CHIPS_TIME_MAX_SEC seconds.
 */
typedef struct {
    uint32_t tv_sec;  /**< Seconds. */
    uint32_t tv_nsec; /**< Nanoseconds; below #CHIPS_NSEC_PER_SEC. */
} chips_time_t;

/** @brief true if @p t has tv_nsec below #CHIPS_NSEC_PER_SEC. */
bool chips_time_valid(chips_time_t t);

/**
 * @brief Wrapping-safe difference @p now − @p since.
 *
 * @param now   Clock reading; valid.
 * @param since Earlier clock reading; valid.
 *
 * @return The elapsed time, with tv_sec taken modulo 2^32.
 */
chips_time_t chips_time_sub(chips_time_t now, chips_time_t since);

/**
 * @brief Compare two durations.
 *
 * @return Negative, zero or positive as @p a is shorter than, equal to or
 *         longer than @p b.
 */
int chips_time_cmp(chips_time_t a, chips_time_t b);

/* ------------------------------------------------------------------------- */
/* Timing                                                                    */
/* ------------------------------------------------------------------------- */

/**
 * @brief Retransmission timing, shared by a master and its slave.
 *
 * These are RESEND_PERIOD and MAX_RESENDS of docs/PROTOCOL.md §3, which
 * defines resend slots, the deadline and the slave's cache window. Configure
 * both sides with the same values.
 */
typedef struct {
    chips_time_t resend_period; /**< Slot length; at least one round trip, see chips_uart_resend_period(). */
    uint8_t max_resends;        /**< Copies allowed after the first; 0 sends the request once. */
} chips_timing_t;

/**
 * @brief Check a timing configuration.
 *
 * @return true if @p timing is non-NULL, @p resend_period is valid and
 *         nonzero, and chips_timing_cache_window() does not exceed
 *         #CHIPS_TIME_MAX_SEC seconds.
 */
bool chips_timing_valid(const chips_timing_t* timing);

/**
 * @brief CACHE_WINDOW of docs/PROTOCOL.md §3:
 *        (max_resends + 2) × resend_period.
 *
 * @param timing Valid configuration (see chips_timing_valid()).
 */
chips_time_t chips_timing_cache_window(const chips_timing_t* timing);

/** @brief Character format of a UART link. */
typedef struct {
    uint32_t baud;     /**< Bit rate in bit/s; nonzero. */
    uint8_t data_bits; /**< Data bits per character; at least 8. */
    bool parity;       /**< A parity bit is sent. */
    uint8_t stop_bits; /**< 1 or 2 (round 1.5 up to 2). */
} chips_uart_params_t;

/**
 * @brief Smallest safe resend period for a UART link.
 *
 * The formula of docs/PROTOCOL.md §3.1: one worst-case round trip (a request
 * and a response of #CHIPS_WIRE_MAX bytes each, one start bit and the
 * configured data, parity and stop bits per byte) plus @p app_latency.
 *
 * @param uart        Link parameters.
 * @param app_latency Everything that is not time on the wire: the slave's
 *                    worst-case handling time, the step periods of both
 *                    sides, and driver or FIFO delays. Valid.
 *
 * @return The period, rounded up to the next nanosecond and capped at
 *         #CHIPS_TIME_MAX_SEC seconds.
 */
chips_time_t chips_uart_resend_period(const chips_uart_params_t* uart, chips_time_t app_latency);

#ifdef __cplusplus
}
#endif

#endif /* CHIPS_CORE_H */
