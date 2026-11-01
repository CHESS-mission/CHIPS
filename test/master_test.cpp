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

TEST_F(MasterTest, InitStartsIdleWithPendingResult) {
    EXPECT_EQ(CHIPS_MASTER_IDLE, State());
    EXPECT_EQ(CHIPS_OUTCOME_PENDING, Result().outcome);
    EXPECT_EQ(nullptr, Result().payload);
}

TEST_F(MasterTest, InitDoesNotTouchThePort) {
    EXPECT_EQ(0, m_fake.write_calls);
    EXPECT_EQ(0, m_fake.read_calls);
}

TEST_F(MasterTest, ResultPayloadPointsIntoResponseBuffer) {
    Transact(0x01, {9, 8, 7});
    EXPECT_EQ(m_rbuf, Result().payload);
    EXPECT_EQ(0, std::memcmp(m_rbuf, "\x09\x08\x07", 3));
}

TEST_F(MasterTest, IdleTrafficNeverWritesTheResponseBuffer) {
    m_fake.Feed(Resp(1, 0x01, Bytes(CHIPS_MAX_PAYLOAD, 0x11)));
    EXPECT_TRUE(Has(StepUntil(CHIPS_EV_RX_DROPPED), CHIPS_EV_RX_DROPPED));
    EXPECT_EQ(Bytes(CHIPS_MAX_PAYLOAD, K_SENTINEL), BufferContents());
}

TEST_F(MasterTest, ResponseStaysValidWhileIdle) {
    Transact(0x01, {1, 2, 3});
    const Bytes before = BufferContents();

    m_fake.Feed(Resp(Seq(), Cmd(), Bytes(CHIPS_MAX_PAYLOAD, 0x22)));
    m_fake.Feed(Resp(static_cast<uint8_t>(Seq() + 1), Cmd(), {4, 5}));
    for (int i = 0; i < 20; ++i) {
        Step();
    }
    EXPECT_TRUE(m_fake.rx.empty());
    EXPECT_EQ(before, BufferContents());
    EXPECT_EQ((Bytes{1, 2, 3}), ResultPayload());
}

TEST_F(MasterTest, SubmitAndCancelDoNotWriteTheResponseBuffer) {
    Submit(0x01, {1, 2, 3});
    EXPECT_TRUE(chips_master_cancel(&m_master));
    EXPECT_EQ(Bytes(CHIPS_MAX_PAYLOAD, K_SENTINEL), BufferContents());
}

TEST_F(MasterTest, SubmitPayloadMayAliasResponseBuffer) {
    const Bytes first = Pattern(200);
    Transact(0x01, first);

    ASSERT_EQ(CHIPS_SUBMIT_OK, chips_master_submit(&m_master, 0x02, m_rbuf, first.size(), m_rbuf));
    m_fake.tx.clear();
    m_fake.Feed(Response(Bytes(CHIPS_MAX_PAYLOAD, 0x33)));
    EXPECT_TRUE(Has(StepUntil(CHIPS_EV_DONE), CHIPS_EV_DONE));

    EXPECT_EQ(Req(Seq(), 0x02, first), m_fake.tx);
    EXPECT_EQ(Bytes(CHIPS_MAX_PAYLOAD, 0x33), ResultPayload());
}

TEST_F(MasterTest, NonOkOutcomeHasNoPayload) {
    Submit(0x01);
    Step();
    EXPECT_TRUE(Has(StepToDeadline(), CHIPS_EV_DONE));
    EXPECT_EQ(CHIPS_OUTCOME_TIMEOUT, Result().outcome);
    EXPECT_EQ(nullptr, Result().payload);
    EXPECT_EQ(0u, Result().payload_len);
}

TEST_F(MasterTest, EachSubmitWritesOnlyItsOwnBuffer) {
    uint8_t other[CHIPS_MAX_PAYLOAD];
    std::memset(other, 0, sizeof(other));

    Transact(0x01, {1, 2}, other);
    EXPECT_EQ(other, Result().payload);
    EXPECT_EQ((Bytes{1, 2}), ResultPayload());
    EXPECT_EQ(Bytes(CHIPS_MAX_PAYLOAD, K_SENTINEL), BufferContents());

    const Bytes other_after = Contents(other);
    Transact(0x05, {7, 7, 7});
    EXPECT_EQ(m_rbuf, Result().payload);
    EXPECT_EQ((Bytes{7, 7, 7}), ResultPayload());
    EXPECT_EQ(other_after, Contents(other));
}

TEST_F(MasterTest, CancelReleasesTheBufferImmediately) {
    uint8_t other[CHIPS_MAX_PAYLOAD];
    Submit(0x01, {}, other);
    Step();
    const uint8_t old_seq = Seq();
    ASSERT_TRUE(chips_master_cancel(&m_master));
    std::memset(other, 0x5C, sizeof(other));  // the caller takes it back right away

    Submit(0x01);
    Step();
    m_fake.Feed(Resp(old_seq, 0x01, Bytes(CHIPS_MAX_PAYLOAD, 0x11)));
    m_fake.Feed(Response({4}));
    EXPECT_TRUE(Has(StepUntil(CHIPS_EV_DONE), CHIPS_EV_DONE));

    EXPECT_EQ(CHIPS_OUTCOME_OK, Result().outcome);
    EXPECT_EQ((Bytes{4}), ResultPayload());
    EXPECT_EQ(Bytes(CHIPS_MAX_PAYLOAD, 0x5C), Contents(other));
}

TEST_F(MasterTest, TimedOutBufferIsNotWrittenAfterwards) {
    uint8_t other[CHIPS_MAX_PAYLOAD];
    Submit(0x01, {}, other);
    Step();
    const uint8_t seq = Seq();
    ASSERT_TRUE(Has(StepToDeadline(), CHIPS_EV_DONE));
    std::memset(other, 0x5C, sizeof(other));

    m_fake.Feed(Resp(seq, 0x01, Bytes(CHIPS_MAX_PAYLOAD, 0x11)));
    StepUntil(CHIPS_EV_RX_DROPPED);
    EXPECT_EQ(Bytes(CHIPS_MAX_PAYLOAD, 0x5C), Contents(other));
}

TEST_F(MasterTest, ReinitReleasesTheBuffer) {
    uint8_t other[CHIPS_MAX_PAYLOAD];
    Submit(0x01, {}, other);
    Step();
    chips_master_init(&m_master, &m_port, &m_timing);
    EXPECT_EQ(CHIPS_MASTER_IDLE, State());
    std::memset(other, 0x5C, sizeof(other));

    m_fake.Feed(Resp(1, 0x01, Bytes(CHIPS_MAX_PAYLOAD, 0x11)));
    StepUntil(CHIPS_EV_RX_DROPPED);
    EXPECT_EQ(Bytes(CHIPS_MAX_PAYLOAD, 0x5C), Contents(other));
}

TEST_F(MasterTest, SubmitStartsWaitingAndRecordsSeqAndCmd) {
    Submit(0x12, {1, 2, 3});
    EXPECT_EQ(CHIPS_MASTER_WAITING, State());
    EXPECT_EQ(CHIPS_OUTCOME_PENDING, Result().outcome);
    EXPECT_EQ(1u, Result().seq);
    EXPECT_EQ(0x12u, Result().cmd);
}

TEST_F(MasterTest, SubmitDoesNotTransmitUntilStepped) {
    Submit(0x01);
    EXPECT_EQ(0, m_fake.write_calls);
    EXPECT_TRUE(m_fake.tx.empty());
}

TEST_F(MasterTest, SubmitWhileWaitingIsBusy) {
    Submit(0x01);
    EXPECT_EQ(CHIPS_SUBMIT_BUSY, chips_master_submit(&m_master, 0x02, nullptr, 0, m_rbuf));
    Step();
    EXPECT_EQ(CHIPS_SUBMIT_BUSY, chips_master_submit(&m_master, 0x02, nullptr, 0, m_rbuf));

    EXPECT_EQ(0x01u, Result().cmd);
    EXPECT_EQ(1u, Result().seq);
}

TEST_F(MasterTest, SubmitRejectsInvalidArguments) {
    const uint8_t one = 0;
    const Bytes too_big(CHIPS_MAX_PAYLOAD + 1, 0xAB);

    if (CHIPS_CMD_MASK < 0xFF) {
        EXPECT_EQ(CHIPS_SUBMIT_INVALID_ARG,
                  chips_master_submit(&m_master, static_cast<uint8_t>(CHIPS_CMD_MASK + 1), &one, 1, m_rbuf));
    }
    EXPECT_EQ(CHIPS_SUBMIT_INVALID_ARG, chips_master_submit(&m_master, 0x01, too_big.data(), too_big.size(), m_rbuf));
    EXPECT_EQ(CHIPS_SUBMIT_INVALID_ARG, chips_master_submit(&m_master, 0x01, nullptr, 1, m_rbuf));
    EXPECT_EQ(CHIPS_SUBMIT_INVALID_ARG, chips_master_submit(&m_master, 0x01, &one, 1, nullptr));

    EXPECT_EQ(CHIPS_MASTER_IDLE, State());
    EXPECT_EQ(CHIPS_OUTCOME_PENDING, Result().outcome);
    Submit(0x01);
    EXPECT_EQ(1u, Seq());
}

TEST_F(MasterTest, SubmitAcceptsBoundaryArguments) {
    const Bytes max_payload(CHIPS_MAX_PAYLOAD, 0x5A);
    EXPECT_EQ(CHIPS_SUBMIT_OK,
              chips_master_submit(&m_master, CHIPS_CMD_MASK, max_payload.data(), max_payload.size(), m_rbuf));
    EXPECT_TRUE(chips_master_cancel(&m_master));

    EXPECT_EQ(CHIPS_SUBMIT_OK, chips_master_submit(&m_master, 0x00, nullptr, 0, m_rbuf));
}

TEST_F(MasterTest, SeqIncrementsPerSubmitAndWrapsAt256) {
    for (int i = 1; i <= 256; ++i) {
        Submit(0x01);
        EXPECT_EQ(static_cast<uint8_t>(i), Seq()) << "submit #" << i;
        ASSERT_TRUE(chips_master_cancel(&m_master));
    }
    Submit(0x01);
    EXPECT_EQ(1u, Seq());
}

TEST_F(MasterTest, TransactionWorksWithWrappedSeqZero) {
    for (int i = 0; i < 255; ++i) {
        Submit(0x01);
        ASSERT_TRUE(chips_master_cancel(&m_master));
    }
    Submit(0x07);
    ASSERT_EQ(0u, Seq());
    Step();
    m_fake.Feed(Response({0x42}));
    EXPECT_TRUE(Has(Step(), CHIPS_EV_DONE));
    EXPECT_EQ(CHIPS_OUTCOME_OK, Result().outcome);
    EXPECT_EQ(0u, Result().seq);
}

TEST_F(MasterTest, SubmitResetsPreviousResult) {
    Transact(0x01, {1, 2});

    Submit(0x02);
    EXPECT_EQ(CHIPS_OUTCOME_PENDING, Result().outcome);
    EXPECT_EQ(nullptr, Result().payload);
    EXPECT_EQ(0u, Result().payload_len);
    EXPECT_EQ(Ms(0), Result().latency);
    EXPECT_EQ(0x02u, Result().cmd);
}

TEST_F(MasterTest, MatchingResponseCompletesWithPayloadAndLatency) {
    m_fake.now = Ms(100);
    Submit(0x21, {7});
    Step();

    m_fake.now = Ms(1334) + Ns(5);
    m_fake.Feed(Response({9, 8, 7}));
    const uint32_t ev = Step();

    EXPECT_TRUE(Has(ev, CHIPS_EV_DONE));
    EXPECT_EQ(CHIPS_MASTER_IDLE, State());
    EXPECT_EQ(CHIPS_OUTCOME_OK, Result().outcome);
    EXPECT_EQ(Ms(1234) + Ns(5), Result().latency);
    EXPECT_EQ((Bytes{9, 8, 7}), ResultPayload());
}

TEST_F(MasterTest, EmptyResponsePayload) {
    Submit(0x01);
    Step();
    m_fake.Feed(Response());
    EXPECT_TRUE(Has(Step(), CHIPS_EV_DONE));
    EXPECT_EQ(CHIPS_OUTCOME_OK, Result().outcome);
    EXPECT_EQ(0u, Result().payload_len);
}

TEST_F(MasterTest, MaxSizeResponsePayload) {
    const Bytes big = Pattern(CHIPS_MAX_PAYLOAD, 31u);
    Submit(0x01);
    Step();
    m_fake.Feed(Response(big));
    EXPECT_TRUE(Has(StepUntil(CHIPS_EV_DONE), CHIPS_EV_DONE));
    EXPECT_EQ(CHIPS_OUTCOME_OK, Result().outcome);
    EXPECT_EQ(big, ResultPayload());
    EXPECT_EQ(big, BufferContents());
}

TEST_F(MasterTest, ResponseSplitAcrossManyReads) {
    m_fake.max_read = 1;
    Submit(0x01);
    Step();
    const Bytes resp = Response({1, 2, 3, 4, 5});
    m_fake.Feed(resp);

    const unsigned long steps_before = m_tally.steps;
    const uint32_t ev = StepUntil(CHIPS_EV_DONE);
    EXPECT_TRUE(Has(ev, CHIPS_EV_DONE));
    EXPECT_FALSE(Has(ev, CHIPS_EV_RX_DROPPED));
    EXPECT_EQ(CHIPS_OUTCOME_OK, Result().outcome);
    EXPECT_EQ((Bytes{1, 2, 3, 4, 5}), ResultPayload());
    EXPECT_EQ(resp.size(), m_tally.steps - steps_before);
}

TEST_F(MasterTest, LatencyIsReportedPerTransaction) {
    Submit(0x01);
    Step();
    m_fake.now += Ms(500);
    m_fake.Feed(Response());
    Step();
    EXPECT_EQ(Ms(500), Result().latency);

    Submit(0x01);
    Step();
    m_fake.now += Ms(200);
    m_fake.Feed(Response());
    Step();
    EXPECT_EQ(Ms(200), Result().latency);
}

TEST_F(MasterTest, CancelWhenIdleReturnsFalse) {
    EXPECT_FALSE(chips_master_cancel(&m_master));
    EXPECT_EQ(CHIPS_OUTCOME_PENDING, Result().outcome);
}

TEST_F(MasterTest, CancelBeforeFirstStepNeverTransmits) {
    Submit(0x01);
    EXPECT_TRUE(chips_master_cancel(&m_master));
    EXPECT_EQ(CHIPS_MASTER_IDLE, State());
    EXPECT_EQ(CHIPS_OUTCOME_CANCELLED, Result().outcome);
    EXPECT_EQ(Ms(0), Result().latency);

    Step();
    EXPECT_EQ(0, m_fake.write_calls);
    EXPECT_FALSE(chips_master_cancel(&m_master));
}

TEST_F(MasterTest, CancelWaitingTransaction) {
    Submit(0x01);
    Step();
    m_fake.now = Ms(500);
    EXPECT_TRUE(chips_master_cancel(&m_master));
    EXPECT_EQ(CHIPS_OUTCOME_CANCELLED, Result().outcome);
    EXPECT_EQ(Ms(0), Result().latency);
    EXPECT_EQ(nullptr, Result().payload);
}

TEST_F(MasterTest, CancelWithFrameInFlightAbortsTx) {
    m_fake.max_write = 2;
    Submit(0x01, {1, 2, 3, 4});
    Step();
    ASSERT_EQ(2u, m_fake.tx.size());

    EXPECT_TRUE(chips_master_cancel(&m_master));

    m_fake.max_write = std::numeric_limits<size_t>::max();
    m_fake.tx.clear();
    Submit(0x02);
    EXPECT_TRUE(Has(Step(), CHIPS_EV_TX_FRAME));
    EXPECT_EQ(Req(Seq(), 0x02), m_fake.tx);
}

TEST_F(MasterTest, IdleStepWithNothingToReadIsQuiet) {
    EXPECT_EQ(static_cast<uint32_t>(CHIPS_EV_NONE), Step());
    EXPECT_EQ(1, m_fake.read_calls);
    EXPECT_EQ(0, m_fake.write_calls);
}

TEST_F(MasterTest, IdleDropsIncomingBytes) {
    m_fake.Feed({1, 2, 3, 4, 5});
    EXPECT_TRUE(Has(Step(), CHIPS_EV_RX_DROPPED));
    EXPECT_TRUE(m_fake.rx.empty());
}

TEST_F(MasterTest, IdleReadErrorIsReported) {
    m_fake.fail_read = true;
    EXPECT_TRUE(Has(Step(), CHIPS_EV_RX_ERROR));
    EXPECT_EQ(CHIPS_MASTER_IDLE, State());
}

TEST_F(MasterTest, ResponseAfterCompletionIsDroppedWhileIdle) {
    Transact(0x01, {6});

    m_fake.Feed(Response());
    EXPECT_TRUE(Has(Step(), CHIPS_EV_RX_DROPPED));
    EXPECT_FALSE(Has(Step(), CHIPS_EV_DONE));
    EXPECT_EQ(1u, Count(CHIPS_EV_DONE));
    EXPECT_EQ(CHIPS_OUTCOME_OK, Result().outcome);
    EXPECT_EQ((Bytes{6}), ResultPayload());
}

TEST_F(MasterTest, EveryStepReadsOnce) {
    Step();
    Submit(0x01);
    Step();
    Step();
    chips_master_cancel(&m_master);
    Step();
    EXPECT_EQ(4, m_fake.read_calls);
}

TEST_F(MasterTest, BackToBackTransactions) {
    for (uint8_t cmd = 1; cmd <= 5; ++cmd) {
        Submit(cmd, {cmd});
        Step();
        m_fake.Feed(Response({static_cast<uint8_t>(cmd * 2)}));
        ASSERT_TRUE(Has(Step(), CHIPS_EV_DONE));
        EXPECT_EQ(CHIPS_OUTCOME_OK, Result().outcome);
        EXPECT_EQ(cmd, Result().seq);
        EXPECT_EQ((Bytes{static_cast<uint8_t>(cmd * 2)}), ResultPayload());
    }
    EXPECT_EQ(5u, Count(CHIPS_EV_DONE));
    EXPECT_EQ(5u, Count(CHIPS_EV_TX_FRAME));
}

}  // namespace
