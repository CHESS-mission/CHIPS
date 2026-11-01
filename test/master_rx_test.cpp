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

TEST_F(MasterTest, StaleSeqResponseIsIgnored) {
    Submit(0x01);
    Step();
    m_fake.Feed(Resp(static_cast<uint8_t>(Seq() - 1), Cmd(), {0xEE}));

    const uint32_t ev = Step();
    EXPECT_TRUE(Has(ev, CHIPS_EV_RX_STALE));
    EXPECT_FALSE(Has(ev, CHIPS_EV_DONE));
    EXPECT_EQ(CHIPS_MASTER_WAITING, State());

    m_fake.Feed(Response({0x11}));
    EXPECT_TRUE(Has(Step(), CHIPS_EV_DONE));
    EXPECT_EQ(CHIPS_OUTCOME_OK, Result().outcome);
    EXPECT_EQ((Bytes{0x11}), ResultPayload());
}

TEST_F(MasterTest, StaleThenValidResponseInOneRead) {
    Submit(0x01);
    Step();
    m_fake.Feed(Resp(static_cast<uint8_t>(Seq() + 5), Cmd()));
    m_fake.Feed(Response({0x33}));

    const uint32_t ev = Step();
    EXPECT_TRUE(Has(ev, CHIPS_EV_RX_STALE));
    EXPECT_TRUE(Has(ev, CHIPS_EV_DONE));
    EXPECT_EQ(CHIPS_OUTCOME_OK, Result().outcome);
    EXPECT_EQ((Bytes{0x33}), ResultPayload());
}

TEST_F(MasterTest, LateResponseToCancelledTransactionIsStale) {
    Submit(0x01);
    Step();
    const uint8_t old_seq = Seq();
    ASSERT_TRUE(chips_master_cancel(&m_master));

    Submit(0x01);
    Step();
    m_fake.Feed(Resp(old_seq, 0x01));
    EXPECT_TRUE(Has(Step(), CHIPS_EV_RX_STALE));
    EXPECT_EQ(CHIPS_MASTER_WAITING, State());
}

TEST_F(MasterTest, CmdMismatchIsIgnoredAndTransactionContinues) {
    Submit(0x01);
    Step();
    m_fake.Feed(Resp(Seq(), 0x02, {1}));

    const uint32_t ev = Step();
    EXPECT_TRUE(Has(ev, CHIPS_EV_RX_CMD_MISMATCH));
    EXPECT_FALSE(Has(ev, CHIPS_EV_RX_STALE));
    EXPECT_FALSE(Has(ev, CHIPS_EV_DONE));
    EXPECT_EQ(CHIPS_MASTER_WAITING, State());
    EXPECT_EQ(CHIPS_OUTCOME_PENDING, Result().outcome);

    m_fake.Feed(Response({2}));
    EXPECT_TRUE(Has(Step(), CHIPS_EV_DONE));
    EXPECT_EQ(CHIPS_OUTCOME_OK, Result().outcome);
    EXPECT_EQ((Bytes{2}), ResultPayload());
}

TEST_F(MasterTest, CmdMismatchOnlyEndsInTimeout) {
    Submit(0x01);
    Step();
    m_fake.Feed(Resp(Seq(), 0x02, {1}));
    EXPECT_TRUE(Has(Step(), CHIPS_EV_RX_CMD_MISMATCH));

    EXPECT_TRUE(Has(StepToDeadline(), CHIPS_EV_DONE));
    EXPECT_EQ(CHIPS_OUTCOME_TIMEOUT, Result().outcome);
    EXPECT_EQ(nullptr, Result().payload);
    EXPECT_EQ(0u, Result().payload_len);
}

TEST_F(MasterTest, EchoOfOwnRequestIsIgnored) {
    Submit(0x01, {1, 2, 3});
    Step();
    m_fake.Feed(m_fake.tx);

    const uint32_t ev = Step();
    EXPECT_TRUE(Has(ev, CHIPS_EV_RX_REQUEST));
    EXPECT_FALSE(Has(ev, CHIPS_EV_DONE));
    EXPECT_FALSE(Has(ev, CHIPS_EV_RX_STALE));
    EXPECT_EQ(CHIPS_MASTER_WAITING, State());
    EXPECT_EQ(CHIPS_OUTCOME_PENDING, Result().outcome);

    m_fake.Feed(Response({4}));
    EXPECT_TRUE(Has(Step(), CHIPS_EV_DONE));
    EXPECT_EQ(CHIPS_OUTCOME_OK, Result().outcome);
    EXPECT_EQ((Bytes{4}), ResultPayload());
}

TEST_F(MasterTest, EchoThenResponseInOneRead) {
    Submit(0x01);
    Step();
    m_fake.Feed(Request());
    m_fake.Feed(Response({7}));

    const uint32_t ev = Step();
    EXPECT_TRUE(Has(ev, CHIPS_EV_RX_REQUEST));
    EXPECT_TRUE(Has(ev, CHIPS_EV_DONE));
    EXPECT_EQ(CHIPS_OUTCOME_OK, Result().outcome);
    EXPECT_EQ((Bytes{7}), ResultPayload());
}

TEST_F(MasterTest, RequestWithOtherSeqIsReportedAsRequestNotStale) {
    Submit(0x01);
    Step();
    m_fake.Feed(Req(static_cast<uint8_t>(Seq() + 1), Cmd()));

    const uint32_t ev = Step();
    EXPECT_TRUE(Has(ev, CHIPS_EV_RX_REQUEST));
    EXPECT_FALSE(Has(ev, CHIPS_EV_RX_STALE));
    EXPECT_FALSE(Has(ev, CHIPS_EV_DONE));
    EXPECT_EQ(CHIPS_MASTER_WAITING, State());
}

TEST_F(MasterTest, EchoOfEveryResendStillTimesOut) {
    Submit(0x01);
    Step();
    for (int i = 0; i < K_MAX_RESENDS; ++i) {
        m_fake.Feed(Request());
        const uint32_t ev = NextSlot();
        EXPECT_TRUE(Has(ev, CHIPS_EV_RX_REQUEST)) << "resend " << i;
        EXPECT_TRUE(Has(ev, CHIPS_EV_TX_FRAME)) << "resend " << i;
        EXPECT_FALSE(Has(ev, CHIPS_EV_DONE)) << "resend " << i;
    }

    EXPECT_TRUE(Has(StepToDeadline(), CHIPS_EV_DONE));
    EXPECT_EQ(CHIPS_OUTCOME_TIMEOUT, Result().outcome);
    EXPECT_EQ(static_cast<unsigned long>(K_MAX_RESENDS), Count(CHIPS_EV_RX_REQUEST));
}

TEST_F(MasterTest, BytesAfterCompletionInSameReadAreDropped) {
    Submit(0x01);
    Step();
    m_fake.Feed(Response({1}));
    m_fake.Feed({0xA1, 0xA2, 0xA3});

    const uint32_t ev = Step();
    EXPECT_TRUE(Has(ev, CHIPS_EV_DONE));
    EXPECT_TRUE(Has(ev, CHIPS_EV_RX_DROPPED));
    EXPECT_TRUE(m_fake.rx.empty());
    EXPECT_EQ(CHIPS_OUTCOME_OK, Result().outcome);
    EXPECT_EQ((Bytes{1}), ResultPayload());
}

TEST_F(MasterTest, CorruptedFrameIsReportedAndTransactionContinues) {
    Submit(0x01);
    Step();
    Bytes bad = Response({0x41, 0x42, 0x43});
    ASSERT_TRUE(ReplaceFirst(bad, 0x42, 0x44)) << "payload byte not found verbatim in encoded frame";
    m_fake.Feed(bad);

    const uint32_t ev = Step();
    EXPECT_TRUE(Has(ev, CHIPS_EV_RX_BAD_FRAME));
    EXPECT_FALSE(Has(ev, CHIPS_EV_DONE));
    EXPECT_FALSE(Has(ev, CHIPS_EV_RX_STALE));
    EXPECT_EQ(CHIPS_MASTER_WAITING, State());

    m_fake.Feed(Response({0x41, 0x42, 0x43}));
    EXPECT_TRUE(Has(Step(), CHIPS_EV_DONE));
    EXPECT_EQ(CHIPS_OUTCOME_OK, Result().outcome);
}

TEST_F(MasterTest, ReadsAtMostOneChunkPerStep) {
    Submit(0x01);
    Step();
    m_fake.Feed(Bytes(CHIPS_RX_CHUNK + 10, 0x00));
    Step();
    EXPECT_EQ(10u, m_fake.rx.size());
}

TEST_F(MasterTest, ResponseQueuedBeforeFirstSendIsStale) {
    m_fake.now = Ms(50);
    Submit(0x01);
    m_fake.Feed(Response());

    const uint32_t ev = Step();
    EXPECT_TRUE(Has(ev, CHIPS_EV_RX_STALE));
    EXPECT_TRUE(Has(ev, CHIPS_EV_TX_FRAME));
    EXPECT_FALSE(Has(ev, CHIPS_EV_DONE));
    EXPECT_EQ(CHIPS_MASTER_WAITING, State());
    EXPECT_EQ(Request(), m_fake.tx);
}

TEST_F(MasterTest, ReadErrorWhileWaitingIsReportedAndRecoverable) {
    Submit(0x01);
    Step();
    m_fake.fail_read = true;
    EXPECT_TRUE(Has(Step(), CHIPS_EV_RX_ERROR));
    EXPECT_EQ(CHIPS_MASTER_WAITING, State());

    m_fake.fail_read = false;
    m_fake.Feed(Response());
    EXPECT_TRUE(Has(Step(), CHIPS_EV_DONE));
    EXPECT_EQ(CHIPS_OUTCOME_OK, Result().outcome);
}

TEST_F(MasterTest, ReadErrorDiscardsPartiallyReceivedFrame) {
    Submit(0x01);
    Step();
    const Bytes resp = Response({1, 2, 3, 4, 5, 6});
    const size_t half = resp.size() / 2;

    m_fake.Feed(Bytes(resp.begin(), resp.begin() + half));
    Step();
    m_fake.fail_read = true;
    Step();
    m_fake.fail_read = false;
    m_fake.Feed(Bytes(resp.begin() + half, resp.end()));

    EXPECT_FALSE(Has(Step(), CHIPS_EV_DONE));
    EXPECT_EQ(CHIPS_MASTER_WAITING, State());

    m_fake.Feed(Response({1, 2, 3, 4, 5, 6}));
    EXPECT_TRUE(Has(StepUntil(CHIPS_EV_DONE, 5), CHIPS_EV_DONE));
    EXPECT_EQ(CHIPS_OUTCOME_OK, Result().outcome);
}

TEST_F(MasterTest, ReadErrorDoesNotBlockTransmitOrTimeout) {
    m_fake.fail_read = true;
    Submit(0x01);
    const uint32_t ev = Step();
    EXPECT_TRUE(Has(ev, CHIPS_EV_RX_ERROR));
    EXPECT_TRUE(Has(ev, CHIPS_EV_TX_FRAME));

    EXPECT_TRUE(Has(StepToDeadline(), CHIPS_EV_DONE));
    EXPECT_EQ(CHIPS_OUTCOME_TIMEOUT, Result().outcome);
}

}  // namespace
