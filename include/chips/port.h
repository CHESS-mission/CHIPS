/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 EPFL Spacecraft Team
 */

/**
 * @file port.h
 * @brief Byte stream and clock binding shared by the CHIPS master and slave.
 */

#ifndef CHIPS_PORT_H
#define CHIPS_PORT_H

#include <stddef.h>
#include <stdint.h>

#include "chips/core.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Result of a port read or write. */
typedef enum {
    CHIPS_IO_OK = 0, /**< Call succeeded (possibly with 0 bytes moved). */
    CHIPS_IO_ERROR   /**< Driver error. */
} chips_io_status_t;

/**
 * @brief Byte stream and clock a state machine runs on.
 *
 * All three functions must return immediately. write and read are only
 * ever called from the owning state machine's step function; now is also
 * called by chips_master_submit() and chips_slave_respond().
 */
typedef struct {
    /**
     * Write up to @p len bytes. Set @p *written to the number accepted
     * (0..len). A short write is not an error: the rest is retried on the
     * next step.
     */
    chips_io_status_t (*write)(void* ctx, const uint8_t* data, size_t len, size_t* written);

    /**
     * Read up to @p cap bytes. Set @p *nread to the number read; 0 when
     * nothing is pending. "No data" must be CHIPS_IO_OK, not an error.
     */
    chips_io_status_t (*read)(void* ctx, uint8_t* buf, size_t cap, size_t* nread);

    /**
     * Monotonic time, e.g. CLOCK_MONOTONIC truncated to 32-bit seconds.
     * tv_nsec must be below #CHIPS_NSEC_PER_SEC; tv_sec may wrap at 2^32.
     */
    chips_time_t (*now)(void* ctx);

    /** Passed unchanged to the functions above. */
    void* ctx;
} chips_port_t;

#ifdef __cplusplus
}
#endif

#endif /* CHIPS_PORT_H */
