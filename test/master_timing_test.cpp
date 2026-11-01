/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 EPFL Spacecraft Team
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <limits>

#include "master_fixture.hpp"

namespace {

using namespace chips_test;

TEST_F(MasterTest, FirstStepSendsEncodedRequest) {
    Submit(0x12, {1, 2, 3});
    const uint32_t ev = Step();

    EXPECT_EQ(CHIPS_MASTER_WAITING, State());
    EXPECT_TRUE(Has(ev, CHIPS_EV_TX_FRAME));
    EXPECT_EQ(Req(1, 0x12, {1, 2, 3}), m_fake.tx);
    EXPECT_EQ(1, m_fake.write_calls);
}

TEST_F(MasterTest, NoResendBeforePeriodElapses) {
    Submit(0x01);
    Step();
    const int writes = m_fake.write_calls;

    m_fake.now = K_RESEND_PERIOD - Ns(1);
    EXPECT_FALSE(Has(Step(), CHIPS_EV_TX_FRAME));
    EXPECT_EQ(writes, m_fake.write_calls);
}

TEST_F(MasterTest, ResendsEveryPeriodMeasuredFromLastSend) {
    Submit(0x01, {9});
    Step();
    const Bytes frame = m_fake.tx;

    m_fake.now = K_RESEND_PERIOD;
    EXPECT_TRUE(Has(Step(), CHIPS_EV_TX_FRAME));

    m_fake.now = 2 * K_RESEND_PERIOD - Ns(1);
    EXPECT_FALSE(Has(Step(), CHIPS_EV_TX_FRAME));

    m_fake.now = 2 * K_RESEND_PERIOD;
    EXPECT_TRUE(Has(Step(), CHIPS_EV_TX_FRAME));
    EXPECT_EQ(3u, Count(CHIPS_EV_TX_FRAME));

    EXPECT_EQ(Repeat(frame, 3), m_fake.tx);
}

TEST_F(MasterTest, ShortWritesAreContinuedOnLaterSteps) {
    m_fake.max_write = 3;
    Submit(0x05, {1, 2, 3, 4, 5, 6, 7, 8});
    const Bytes expected = Req(1, 0x05, {1, 2, 3, 4, 5, 6, 7, 8});
    ASSERT_GT(expected.size(), 3u);

    uint32_t ev = Step();
    EXPECT_FALSE(Has(ev, CHIPS_EV_TX_FRAME));
    EXPECT_EQ(3u, m_fake.tx.size());

    ev = StepUntil(CHIPS_EV_TX_FRAME, 100);
    EXPECT_TRUE(Has(ev, CHIPS_EV_TX_FRAME));
    EXPECT_EQ(expected, m_fake.tx);
    EXPECT_EQ(1u, Count(CHIPS_EV_TX_FRAME));
    EXPECT_EQ(static_cast<int>((expected.size() + 2) / 3), m_fake.write_calls);
}

TEST_F(MasterTest, InFlightFrameIsNotRestartedByResendTimer) {
    m_fake.max_write = 3;
    Submit(0x05, {1, 2, 3, 4, 5, 6, 7, 8});
    const Bytes expected = Req(1, 0x05, {1, 2, 3, 4, 5, 6, 7, 8});
    ASSERT_GT(expected.size(), 6u);

    Step();
    m_fake.now = 2 * K_RESEND_PERIOD;
    Step();

    ASSERT_EQ(CHIPS_MASTER_WAITING, State());
    EXPECT_EQ(1u, m_master.resends);
    EXPECT_EQ(Bytes(expected.begin(), expected.begin() + 6), m_fake.tx);
}

TEST_F(MasterTest, WriteErrorReportsTxErrorAndRetriesAfterPeriod) {
    m_fake.fail_write = true;
    Submit(0x01);
    const uint32_t ev = Step();
    EXPECT_TRUE(Has(ev, CHIPS_EV_TX_ERROR));
    EXPECT_FALSE(Has(ev, CHIPS_EV_TX_FRAME));
    EXPECT_EQ(CHIPS_MASTER_WAITING, State());

    m_fake.fail_write = false;
    const int writes = m_fake.write_calls;
    m_fake.now = K_RESEND_PERIOD - Ns(1);
    Step();
    EXPECT_EQ(writes, m_fake.write_calls);

    m_fake.now = K_RESEND_PERIOD;
    EXPECT_TRUE(Has(Step(), CHIPS_EV_TX_FRAME));
    EXPECT_EQ(1u, Count(CHIPS_EV_TX_FRAME));
    EXPECT_EQ(Request(), m_fake.tx);
}

TEST_F(MasterTest, WriteErrorMidFrameRestartsWholeFrameOnRetry) {
    m_fake.max_write = 2;
    Submit(0x05, {1, 2, 3, 4});
    const Bytes expected = Req(1, 0x05, {1, 2, 3, 4});

    Step();
    m_fake.fail_write = true;
    EXPECT_TRUE(Has(Step(), CHIPS_EV_TX_ERROR));

    m_fake.fail_write = false;
    m_fake.max_write = std::numeric_limits<size_t>::max();
    m_fake.tx.clear();
    m_fake.now = K_RESEND_PERIOD;
    EXPECT_TRUE(Has(Step(), CHIPS_EV_TX_FRAME));
    EXPECT_EQ(expected, m_fake.tx);
}

TEST_F(MasterTest, TimesOutExactlyAtDeadline) {
    Submit(0x01);
    Step();
    UseAllResends();
    ASSERT_EQ(K_TIMEOUT - K_RESEND_PERIOD, m_fake.now);

    m_fake.now = K_TIMEOUT - Ns(1);
    EXPECT_FALSE(Has(Step(), CHIPS_EV_DONE));
    EXPECT_EQ(CHIPS_MASTER_WAITING, State());

    m_fake.now = K_TIMEOUT;
    EXPECT_TRUE(Has(Step(), CHIPS_EV_DONE));
    EXPECT_EQ(CHIPS_MASTER_IDLE, State());
    EXPECT_EQ(CHIPS_OUTCOME_TIMEOUT, Result().outcome);
    EXPECT_EQ(K_TIMEOUT, Result().latency);
}

TEST_F(MasterTest, TimeoutAndLatencyAreMeasuredFromSubmit) {
    m_fake.now = Ms(5000);
    Submit(0x01);
    m_fake.now = Ms(5300);
    Step();
    UseAllResends();
    m_fake.now = Ms(5000) + K_TIMEOUT - Ns(1);
    EXPECT_FALSE(Has(Step(), CHIPS_EV_DONE));
    m_fake.now = Ms(5000) + K_TIMEOUT;
    EXPECT_TRUE(Has(Step(), CHIPS_EV_DONE));
    EXPECT_EQ(CHIPS_OUTCOME_TIMEOUT, Result().outcome);
    EXPECT_EQ(K_TIMEOUT, Result().latency);
}

TEST_F(MasterTest, TimeoutCountsSlotsNotElapsedTime) {
    Submit(0x01);
    Step();

    m_fake.now = 10 * K_TIMEOUT;
    const uint32_t ev = Step();
    EXPECT_FALSE(Has(ev, CHIPS_EV_DONE));
    EXPECT_TRUE(Has(ev, CHIPS_EV_TX_FRAME));
    EXPECT_EQ(1u, m_master.resends);

    EXPECT_TRUE(Has(StepToDeadline(), CHIPS_EV_DONE));
    EXPECT_EQ(CHIPS_OUTCOME_TIMEOUT, Result().outcome);
    EXPECT_EQ(10 * K_TIMEOUT + K_MAX_RESENDS * K_RESEND_PERIOD, Result().latency);
}

TEST_F(MasterTest, SendsOneCopyPerSlotUpToMaxResends) {
    Submit(0x01);
    Step();
    EXPECT_TRUE(Has(StepToDeadline(), CHIPS_EV_DONE));
    EXPECT_EQ(K_MAX_RESENDS + 1u, Count(CHIPS_EV_TX_FRAME));
    EXPECT_EQ(Repeat(Request(), K_MAX_RESENDS + 1), m_fake.tx);
}

TEST_F(MasterTest, ConfiguredTimingIsUsed) {
    m_timing.resend_period = Ms(250);
    m_timing.max_resends = 3;
    chips_master_init(&m_master, &m_port, &m_timing);
    Submit(0x01);
    Step();

    m_fake.now = Ms(250) - Ns(1);
    EXPECT_FALSE(Has(Step(), CHIPS_EV_TX_FRAME));
    for (uint32_t t = 250; t < 1000; t += 250) {
        m_fake.now = Ms(t);
        EXPECT_TRUE(Has(Step(), CHIPS_EV_TX_FRAME)) << "t=" << t;
    }
    m_fake.now = Ms(1000);
    EXPECT_TRUE(Has(Step(), CHIPS_EV_DONE));
    EXPECT_EQ(CHIPS_OUTCOME_TIMEOUT, Result().outcome);
    EXPECT_EQ(Ms(1000), Result().latency);
    EXPECT_EQ(4u, Count(CHIPS_EV_TX_FRAME));
}

TEST_F(MasterTest, ZeroResendsSendsOnceAndTimesOutAfterOnePeriod) {
    m_timing.max_resends = 0;
    chips_master_init(&m_master, &m_port, &m_timing);
    Submit(0x01);
    EXPECT_TRUE(Has(Step(), CHIPS_EV_TX_FRAME));

    m_fake.now = K_RESEND_PERIOD;
    const uint32_t ev = Step();
    EXPECT_TRUE(Has(ev, CHIPS_EV_DONE));
    EXPECT_FALSE(Has(ev, CHIPS_EV_TX_FRAME));
    EXPECT_EQ(CHIPS_OUTCOME_TIMEOUT, Result().outcome);
    EXPECT_EQ(1u, Count(CHIPS_EV_TX_FRAME));
}

TEST_F(MasterTest, ResponseArrivingAtDeadlineWins) {
    Submit(0x01);
    Step();
    UseAllResends();
    m_fake.now = m_master.t_slot + K_RESEND_PERIOD;
    m_fake.Feed(Response({5}));
    EXPECT_TRUE(Has(Step(), CHIPS_EV_DONE));
    EXPECT_EQ(CHIPS_OUTCOME_OK, Result().outcome);
}

TEST_F(MasterTest, NoTransmitOnTheStepThatTimesOut) {
    Submit(0x01);
    Step();
    UseAllResends();
    const int writes = m_fake.write_calls;
    const uint32_t ev = StepToDeadline();
    EXPECT_TRUE(Has(ev, CHIPS_EV_DONE));
    EXPECT_FALSE(Has(ev, CHIPS_EV_TX_FRAME));
    EXPECT_EQ(writes, m_fake.write_calls);
}

TEST_F(MasterTest, SendFailedWhenNoFrameEverWentOut) {
    m_fake.fail_write = true;
    Submit(0x01);
    StepUntil(CHIPS_EV_TX_ERROR);
    EXPECT_TRUE(Has(StepToDeadline(), CHIPS_EV_DONE));
    EXPECT_EQ(CHIPS_OUTCOME_SEND_FAILED, Result().outcome);
}

TEST_F(MasterTest, TimeoutAfterOneSuccessfulSendIsTimeoutEvenIfResendsFail) {
    Submit(0x01);
    Step();
    m_fake.fail_write = true;
    EXPECT_TRUE(Has(NextSlot(), CHIPS_EV_TX_ERROR));
    EXPECT_TRUE(Has(StepToDeadline(), CHIPS_EV_DONE));
    EXPECT_EQ(CHIPS_OUTCOME_TIMEOUT, Result().outcome);
}

TEST_F(MasterTest, TimeoutWithFrameStuckInFlightAbortsTx) {
    m_fake.max_write = 0;
    Submit(0x01);
    Step();
    EXPECT_EQ(1, m_fake.write_calls);
    EXPECT_TRUE(m_fake.tx.empty());

    EXPECT_TRUE(Has(StepToDeadline(), CHIPS_EV_DONE));
    EXPECT_EQ(CHIPS_OUTCOME_SEND_FAILED, Result().outcome);

    m_fake.max_write = std::numeric_limits<size_t>::max();
    const int writes = m_fake.write_calls;
    Step();
    EXPECT_EQ(writes, m_fake.write_calls);
    Submit(0x02);
    EXPECT_TRUE(Has(Step(), CHIPS_EV_TX_FRAME));
    EXPECT_EQ(Req(Seq(), 0x02), m_fake.tx);
}

TEST_F(MasterTest, TimeoutWorksAcrossClockWraparound) {
    /* 100 ms before tv_sec wraps from 2^32 - 1 to 0. */
    const chips_time_t start = Ms(0) - Ms(100);
    ASSERT_EQ(std::numeric_limits<uint32_t>::max(), start.tv_sec);
    m_fake.now = start;
    Submit(0x01);
    Step();
    UseAllResends();

    m_fake.now = start + K_TIMEOUT - Ns(1);
    EXPECT_FALSE(Has(Step(), CHIPS_EV_DONE));
    m_fake.now = start + K_TIMEOUT;
    EXPECT_TRUE(Has(Step(), CHIPS_EV_DONE));
    EXPECT_EQ(CHIPS_OUTCOME_TIMEOUT, Result().outcome);
    EXPECT_EQ(K_TIMEOUT, Result().latency);
}

TEST_F(MasterTest, LatencyWorksAcrossClockWraparound) {
    m_fake.now = Ms(0) - Ms(100);
    Submit(0x01);
    Step();
    m_fake.now = Ms(100);
    m_fake.Feed(Response());
    EXPECT_TRUE(Has(Step(), CHIPS_EV_DONE));
    EXPECT_EQ(Ms(200), Result().latency);
}

}  // namespace
