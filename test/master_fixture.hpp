/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 EPFL Spacecraft Team
 */

#ifndef CHIPS_MASTER_FIXTURE_H
#define CHIPS_MASTER_FIXTURE_H

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>

#include "chips/master.h"
#include "test_util.hpp"

namespace chips_test {

constexpr uint8_t K_SENTINEL = 0xE5;

class MasterTest : public ::testing::Test {
  protected:
    void SetUp() override {
        m_port = m_fake.Port();
        FillBuffer(K_SENTINEL);
        chips_master_init(&m_master, &m_port, &m_timing);
    }

    uint32_t Step() {
        const uint32_t ev = chips_master_step(&m_master);
        m_tally.Add(ev);
        return ev;
    }

    uint32_t StepUntil(uint32_t flag, int max_steps = 1000) {
        return chips_test::StepUntil([this] { return Step(); }, flag, max_steps);
    }

    unsigned long Count(uint32_t flag) const { return m_tally.Count(flag); }

    /* Steps exactly one resend period after the current slot started. */
    uint32_t NextSlot() {
        m_fake.now = m_master.t_slot + m_timing.resend_period;
        return Step();
    }

    /* Opens every resend slot that is left, stopping just before the deadline. */
    void UseAllResends() {
        while (State() == CHIPS_MASTER_WAITING && m_master.resends < m_timing.max_resends) {
            NextSlot();
        }
    }

    /* Steps through the remaining slots; returns the events of the deadline step. */
    uint32_t StepToDeadline() {
        UseAllResends();
        return NextSlot();
    }

    void Submit(uint8_t cmd, const Bytes& payload = {}, uint8_t* buf = nullptr) {
        ASSERT_EQ(CHIPS_SUBMIT_OK, chips_master_submit(&m_master, cmd, payload.empty() ? nullptr : payload.data(),
                                                       payload.size(), buf ? buf : m_rbuf));
    }

    void Transact(uint8_t cmd, const Bytes& payload, uint8_t* buf = nullptr) {
        Submit(cmd, {}, buf);
        Step();
        m_fake.Feed(Response(payload));
        ASSERT_TRUE(Has(StepUntil(CHIPS_EV_DONE), CHIPS_EV_DONE));
        ASSERT_EQ(CHIPS_OUTCOME_OK, Result().outcome);
    }

    uint8_t Seq() const { return Result().seq; }
    uint8_t Cmd() const { return Result().cmd; }

    Bytes Response(const Bytes& payload = {}) const { return Resp(Seq(), Cmd(), payload); }
    Bytes Request() const { return Req(Seq(), Cmd()); }

    const chips_master_result_t& Result() const { return *chips_master_result(&m_master); }
    chips_master_state_t State() const { return chips_master_state(&m_master); }

    Bytes ResultPayload() const {
        const auto& r = Result();
        return r.payload_len ? Bytes(r.payload, r.payload + r.payload_len) : Bytes{};
    }

    void FillBuffer(uint8_t v) { std::memset(m_rbuf, v, sizeof(m_rbuf)); }
    Bytes BufferContents() const { return Bytes(m_rbuf, m_rbuf + sizeof(m_rbuf)); }

    static Bytes Contents(const uint8_t (&buf)[CHIPS_MAX_PAYLOAD]) { return Bytes(buf, buf + CHIPS_MAX_PAYLOAD); }

    FakePort m_fake;
    chips_port_t m_port{};
    chips_timing_t m_timing = TestTiming();
    uint8_t m_rbuf[CHIPS_MAX_PAYLOAD];
    chips_master_t m_master;
    EventTally m_tally;
};

}  // namespace chips_test

#endif /* CHIPS_MASTER_FIXTURE_H */
