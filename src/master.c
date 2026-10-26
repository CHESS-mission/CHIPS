/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 EPFL Spacecraft Team
 */

#include "chips/master.h"

static uint32_t elapsed_us(uint32_t now, uint32_t since) {
    return (uint32_t)(now - since);
}

static bool tx_in_flight(const chips_master_t* m) {
    return m->tx_off < m->tx_len;
}

static uint32_t finish(chips_master_t* m, chips_outcome_t outcome, uint32_t latency_us) {
    CHIPS_ASSERT(m->state != CHIPS_MASTER_IDLE);
    CHIPS_ASSERT(outcome != CHIPS_OUTCOME_PENDING);

    m->tx_off = m->tx_len;

    m->result.outcome = outcome;
    m->result.latency_us = latency_us;
    if (outcome == CHIPS_OUTCOME_OK) {
        m->result.payload = m->frame.payload;
        m->result.payload_len = m->frame.payload_len;
    }

    m->state = CHIPS_MASTER_IDLE;
    return CHIPS_EV_DONE;
}

static bool read_chunk(chips_master_t* m, size_t* n) {
    *n = 0;
    if (m->port.read(m->port.ctx, m->rx_chunk, sizeof(m->rx_chunk), n) != CHIPS_IO_OK) {
        *n = 0;
        return false;
    }
    CHIPS_ASSERT(*n <= sizeof(m->rx_chunk));
    if (*n > sizeof(m->rx_chunk)) {
        *n = sizeof(m->rx_chunk);
    }
    return true;
}

static uint32_t on_frame(chips_master_t* m, uint32_t now) {
    if (m->frame.direction != CHIPS_DIR_RESPONSE) {
        return CHIPS_EV_RX_REQUEST;
    }
    if (!m->sent_ok || m->frame.seq != m->seq) {
        return CHIPS_EV_RX_STALE;
    }
    if (m->frame.cmd != m->cmd) {
        return CHIPS_EV_RX_CMD_MISMATCH;
    }
    return finish(m, CHIPS_OUTCOME_OK, elapsed_us(now, m->t_start));
}

static uint32_t receive(chips_master_t* m, uint32_t now) {
    uint32_t ev = CHIPS_EV_NONE;
    size_t n = 0;

    if (!read_chunk(m, &n)) {
        (void)chips_decoder_reset(&m->dec);
        return CHIPS_EV_RX_ERROR;
    }

    const uint8_t* p = m->rx_chunk;
    size_t left = n;

    while (left > 0 && m->state == CHIPS_MASTER_WAITING) {
        size_t used = 0;
        const chips_dec_status_t st = chips_decode(&m->dec, p, left, &used, &m->frame);

        CHIPS_ASSERT(used > 0 && used <= left);
        used = used > left ? left : used;

        p += used;
        left -= used;

        switch (st) {
            case CHIPS_DEC_NEED_MORE:
                break;
            case CHIPS_DEC_FRAME:
                ev |= on_frame(m, now);
                break;
            case CHIPS_DEC_ERR_ESCAPE:
            case CHIPS_DEC_ERR_TOO_LONG:
            case CHIPS_DEC_ERR_TOO_SHORT:
            case CHIPS_DEC_ERR_CRC:
                ev |= CHIPS_EV_RX_BAD_FRAME;
                break;
            case CHIPS_DEC_ERR_ARG:
            default:
                CHIPS_ASSERT(0);
                break;
        }
    }

    if (left > 0) {
        ev |= CHIPS_EV_RX_DROPPED;
    }
    return ev;
}

static uint32_t continue_transmission(chips_master_t* m) {
    const size_t want = m->tx_len - m->tx_off;
    size_t written = 0;
    if (m->port.write(m->port.ctx, &m->tx_frame[m->tx_off], want, &written) != CHIPS_IO_OK) {
        m->tx_off = m->tx_len;
        return CHIPS_EV_TX_ERROR;
    }
    CHIPS_ASSERT(written <= want);
    written = written > want ? want : written;

    m->tx_off += written;

    if (written < want) {
        return CHIPS_EV_NONE;
    }

    m->sent_ok = true;
    return CHIPS_EV_TX_FRAME;
}

static uint32_t transmit(chips_master_t* m, uint32_t now) {
    if (!tx_in_flight(m)) {
        if (m->tx_started && elapsed_us(now, m->t_last_send) < m->resend_period_us) {
            return CHIPS_EV_NONE;
        }
        m->tx_started = true;
        m->t_last_send = now;
        m->tx_off = 0;
    }
    return continue_transmission(m);
}

static uint32_t idle_step(chips_master_t* m) {
    size_t n = 0;
    if (!read_chunk(m, &n)) {
        return CHIPS_EV_RX_ERROR;
    }
    return (n > 0) ? CHIPS_EV_RX_DROPPED : CHIPS_EV_NONE;
}

static uint32_t txn_step(chips_master_t* m, uint32_t now) {
    uint32_t ev = receive(m, now);

    if (m->state == CHIPS_MASTER_WAITING && elapsed_us(now, m->t_start) >= m->timeout_us) {
        const chips_outcome_t outcome = m->sent_ok ? CHIPS_OUTCOME_TIMEOUT : CHIPS_OUTCOME_SEND_FAILED;
        ev |= finish(m, outcome, elapsed_us(now, m->t_start));
    }

    if (m->state == CHIPS_MASTER_WAITING) {
        ev |= transmit(m, now);
    }
    return ev;
}

void chips_master_init(chips_master_t* m, const chips_port_t* port, const chips_master_config_t* cfg) {
    CHIPS_ASSERT(m != NULL && port != NULL && cfg != NULL);
    CHIPS_ASSERT(port->write != NULL && port->read != NULL && port->now_us != NULL);
    CHIPS_ASSERT(cfg->resend_period_us > 0 && cfg->resend_period_us <= CHIPS_TIME_MAX_US);

    chips_crc16_kermit_init();

    *m = (chips_master_t){0};
    m->port = *port;
    m->resend_period_us = cfg->resend_period_us;
    m->state = CHIPS_MASTER_IDLE;
    m->result.outcome = CHIPS_OUTCOME_PENDING;
}

chips_submit_status_t chips_master_submit(chips_master_t* m,
                                          uint8_t cmd,
                                          const uint8_t* payload,
                                          size_t payload_len,
                                          uint32_t timeout_us,
                                          uint8_t response_buf[CHIPS_MAX_PAYLOAD]) {
    CHIPS_ASSERT(m != NULL);

    if (m->state != CHIPS_MASTER_IDLE) {
        return CHIPS_SUBMIT_BUSY;
    }
    if (cmd > CHIPS_CMD_MASK || payload_len > CHIPS_MAX_PAYLOAD || (payload == NULL && payload_len > 0) ||
        timeout_us == 0 || timeout_us > CHIPS_TIME_MAX_US || response_buf == NULL) {
        return CHIPS_SUBMIT_INVALID_ARG;
    }

    m->seq = (uint8_t)(m->seq + 1u);
    m->cmd = cmd;
    m->timeout_us = timeout_us;
    m->t_start = 0;
    m->t_last_send = 0;
    m->tx_started = false;
    m->sent_ok = false;

    m->tx_len = chips_encode(m->seq, CHIPS_DIR_REQUEST, cmd, payload, payload_len, m->tx_frame);
    CHIPS_ASSERT(m->tx_len > 0 && m->tx_len <= sizeof(m->tx_frame));
    m->tx_off = m->tx_len;

    chips_decoder_init(&m->dec, response_buf);

    m->result = (chips_master_result_t){0};
    m->result.outcome = CHIPS_OUTCOME_PENDING;
    m->result.seq = m->seq;
    m->result.cmd = cmd;

    m->state = CHIPS_MASTER_ARMED;
    return CHIPS_SUBMIT_OK;
}

uint32_t chips_master_step(chips_master_t* m) {
    CHIPS_ASSERT(m != NULL);

    const uint32_t now = m->port.now_us(m->port.ctx);

    switch (m->state) {
        case CHIPS_MASTER_IDLE:
            return idle_step(m);

        case CHIPS_MASTER_ARMED:
            m->t_start = now;
            m->state = CHIPS_MASTER_WAITING;
            return txn_step(m, now);

        case CHIPS_MASTER_WAITING:
            return txn_step(m, now);

        default:
            CHIPS_ASSERT(0);
            return CHIPS_EV_NONE;
    }
}

bool chips_master_cancel(chips_master_t* m) {
    CHIPS_ASSERT(m != NULL);
    if (m->state == CHIPS_MASTER_IDLE) {
        return false;
    }
    (void)finish(m, CHIPS_OUTCOME_CANCELLED, 0);
    return true;
}

chips_master_state_t chips_master_state(const chips_master_t* m) {
    CHIPS_ASSERT(m != NULL);
    return m->state;
}

const chips_master_result_t* chips_master_result(const chips_master_t* m) {
    CHIPS_ASSERT(m != NULL);
    return &m->result;
}
