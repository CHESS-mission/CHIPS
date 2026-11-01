/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 EPFL Spacecraft Team
 */

#include "chips/master.h"

static uint32_t finish(chips_master_t* m, chips_outcome_t outcome, chips_time_t latency) {
    CHIPS_ASSERT(m->state != CHIPS_MASTER_IDLE);
    CHIPS_ASSERT(outcome != CHIPS_OUTCOME_PENDING);

    chips_link_abort(&m->link);

    m->result.outcome = outcome;
    m->result.latency = latency;
    if (outcome == CHIPS_OUTCOME_OK) {
        const chips_frame_t* f = chips_link_frame(&m->link);
        m->result.payload = f->payload;
        m->result.payload_len = f->payload_len;
    }

    m->state = CHIPS_MASTER_IDLE;
    return CHIPS_EV_DONE;
}

static uint32_t on_frame(chips_master_t* m, chips_time_t now) {
    const chips_frame_t* f = chips_link_frame(&m->link);

    if (f->direction != CHIPS_DIR_RESPONSE) {
        return CHIPS_EV_RX_REQUEST;
    }
    if (!m->sent_ok || f->seq != m->seq) {
        return CHIPS_EV_RX_STALE;
    }
    if (f->cmd != m->cmd) {
        return CHIPS_EV_RX_CMD_MISMATCH;
    }
    return finish(m, CHIPS_OUTCOME_OK, chips_time_sub(now, m->t_start));
}

static uint32_t receive(chips_master_t* m, chips_time_t now) {
    uint32_t ev = CHIPS_EV_NONE;

    if (!chips_link_read(&m->link)) {
        return CHIPS_EV_RX_ERROR;
    }
    while (m->state == CHIPS_MASTER_WAITING) {
        const chips_dec_status_t st = chips_link_next(&m->link);
        if (st == CHIPS_DEC_NEED_MORE) {
            break;
        }
        ev |= (st == CHIPS_DEC_FRAME) ? on_frame(m, now) : CHIPS_EV_RX_BAD_FRAME;
    }
    if (chips_link_unread(&m->link) > 0) {
        ev |= CHIPS_EV_RX_DROPPED;
    }
    return ev;
}

static uint32_t transmit(chips_master_t* m) {
    const uint32_t ev = chips_link_write(&m->link);
    if (ev & CHIPS_EV_TX_FRAME) {
        m->sent_ok = true;
    }
    return ev;
}

static uint32_t next_slot(chips_master_t* m, chips_time_t now) {
    if (chips_time_cmp(chips_time_sub(now, m->t_slot), m->timing.resend_period) < 0) {
        return CHIPS_EV_NONE;
    }
    m->t_slot = now;

    if (m->resends >= m->timing.max_resends) {
        const chips_outcome_t outcome = m->sent_ok ? CHIPS_OUTCOME_TIMEOUT : CHIPS_OUTCOME_SEND_FAILED;
        return finish(m, outcome, chips_time_sub(now, m->t_start));
    }

    m->resends++;
    if (!chips_link_tx_busy(&m->link)) {
        chips_link_restart(&m->link);
    }
    return CHIPS_EV_NONE;
}

static uint32_t idle_step(chips_master_t* m) {
    if (!chips_link_read(&m->link)) {
        return CHIPS_EV_RX_ERROR;
    }
    return (chips_link_unread(&m->link) > 0) ? CHIPS_EV_RX_DROPPED : CHIPS_EV_NONE;
}

static uint32_t txn_step(chips_master_t* m, chips_time_t now) {
    uint32_t ev = receive(m, now);

    if (m->state == CHIPS_MASTER_WAITING) {
        ev |= next_slot(m, now);
    }
    if (m->state == CHIPS_MASTER_WAITING) {
        ev |= transmit(m);
    }
    return ev;
}

void chips_master_init(chips_master_t* m, const chips_port_t* port, const chips_timing_t* timing) {
    CHIPS_ASSERT(m != NULL);
    CHIPS_ASSERT(chips_timing_valid(timing));

    *m = (chips_master_t){0};
    chips_link_init(&m->link, port);
    m->timing = *timing;
}

chips_submit_status_t chips_master_submit(chips_master_t* m,
                                          uint8_t cmd,
                                          const uint8_t* payload,
                                          size_t payload_len,
                                          uint8_t response_buf[CHIPS_MAX_PAYLOAD]) {
    CHIPS_ASSERT(m != NULL);

    if (m->state != CHIPS_MASTER_IDLE) {
        return CHIPS_SUBMIT_BUSY;
    }
    const uint8_t seq = (uint8_t)(m->seq + 1u);
    if (response_buf == NULL || !chips_link_encode(&m->link, seq, CHIPS_DIR_REQUEST, cmd, payload, payload_len)) {
        return CHIPS_SUBMIT_INVALID_ARG;
    }
    chips_link_rx_attach(&m->link, response_buf);
    chips_link_restart(&m->link);

    m->seq = seq;
    m->cmd = cmd;
    m->resends = 0;
    m->sent_ok = false;

    m->result = (chips_master_result_t){0};
    m->result.seq = seq;
    m->result.cmd = cmd;

    m->t_start = chips_link_now(&m->link);
    m->t_slot = m->t_start;
    m->state = CHIPS_MASTER_WAITING;
    return CHIPS_SUBMIT_OK;
}

uint32_t chips_master_step(chips_master_t* m) {
    CHIPS_ASSERT(m != NULL);

    if (m->state == CHIPS_MASTER_IDLE) {
        return idle_step(m);
    }
    return txn_step(m, chips_link_now(&m->link));
}

bool chips_master_cancel(chips_master_t* m) {
    CHIPS_ASSERT(m != NULL);
    if (m->state == CHIPS_MASTER_IDLE) {
        return false;
    }
    (void)finish(m, CHIPS_OUTCOME_CANCELLED, (chips_time_t){0});
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
