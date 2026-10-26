/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 EPFL Spacecraft Team
 */

#include "chips/master.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <limits>

#include "test_util.hpp"

namespace {

using namespace chips_test;

constexpr uint32_t K_RESEND_US = 1000;
constexpr uint32_t K_TIMEOUT_US = 10000;
constexpr uint8_t K_SENTINEL = 0xE5;

class MasterTest : public ::testing::Test {
  protected:
    void SetUp() override {
        m_port = m_fake.Port();
        m_cfg = chips_master_config_t{};
        m_cfg.resend_period_us = K_RESEND_US;
        FillBuffer(K_SENTINEL);
        chips_master_init(&m_master, &m_port, &m_cfg);
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

    void Submit(uint8_t cmd, const Bytes& payload = {}, uint32_t timeout = K_TIMEOUT_US, uint8_t* buf = nullptr) {
        ASSERT_EQ(CHIPS_SUBMIT_OK, chips_master_submit(&m_master, cmd, payload.empty() ? nullptr : payload.data(),
                                                       payload.size(), timeout, buf ? buf : m_rbuf));
    }

    void Transact(uint8_t cmd, const Bytes& payload, uint8_t* buf = nullptr) {
        Submit(cmd, {}, K_TIMEOUT_US, buf);
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
    chips_master_config_t m_cfg{};
    uint8_t m_rbuf[CHIPS_MAX_PAYLOAD];
    chips_master_t m_master;
    EventTally m_tally;
};

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

    ASSERT_EQ(CHIPS_SUBMIT_OK, chips_master_submit(&m_master, 0x02, m_rbuf, first.size(), K_TIMEOUT_US, m_rbuf));
    m_fake.tx.clear();
    m_fake.Feed(Response(Bytes(CHIPS_MAX_PAYLOAD, 0x33)));
    EXPECT_TRUE(Has(StepUntil(CHIPS_EV_DONE), CHIPS_EV_DONE));

    EXPECT_EQ(Req(Seq(), 0x02, first), m_fake.tx);
    EXPECT_EQ(Bytes(CHIPS_MAX_PAYLOAD, 0x33), ResultPayload());
}

TEST_F(MasterTest, NonOkOutcomeHasNoPayload) {
    Submit(0x01);
    Step();
    m_fake.now = K_TIMEOUT_US;
    EXPECT_TRUE(Has(Step(), CHIPS_EV_DONE));
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
    Submit(0x01, {}, K_TIMEOUT_US, other);
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
    Submit(0x01, {}, K_TIMEOUT_US, other);
    Step();
    const uint8_t seq = Seq();
    m_fake.now = K_TIMEOUT_US;
    ASSERT_TRUE(Has(Step(), CHIPS_EV_DONE));
    std::memset(other, 0x5C, sizeof(other));

    m_fake.Feed(Resp(seq, 0x01, Bytes(CHIPS_MAX_PAYLOAD, 0x11)));
    StepUntil(CHIPS_EV_RX_DROPPED);
    EXPECT_EQ(Bytes(CHIPS_MAX_PAYLOAD, 0x5C), Contents(other));
}

TEST_F(MasterTest, ReinitReleasesTheBuffer) {
    uint8_t other[CHIPS_MAX_PAYLOAD];
    Submit(0x01, {}, K_TIMEOUT_US, other);
    Step();
    chips_master_init(&m_master, &m_port, &m_cfg);
    EXPECT_EQ(CHIPS_MASTER_IDLE, State());
    std::memset(other, 0x5C, sizeof(other));

    m_fake.Feed(Resp(1, 0x01, Bytes(CHIPS_MAX_PAYLOAD, 0x11)));
    StepUntil(CHIPS_EV_RX_DROPPED);
    EXPECT_EQ(Bytes(CHIPS_MAX_PAYLOAD, 0x5C), Contents(other));
}

TEST_F(MasterTest, SubmitArmsAndRecordsSeqAndCmd) {
    Submit(0x12, {1, 2, 3});
    EXPECT_EQ(CHIPS_MASTER_ARMED, State());
    EXPECT_EQ(CHIPS_OUTCOME_PENDING, Result().outcome);
    EXPECT_EQ(1u, Result().seq);
    EXPECT_EQ(0x12u, Result().cmd);
}

TEST_F(MasterTest, SubmitDoesNotTransmitUntilStepped) {
    Submit(0x01);
    EXPECT_EQ(0, m_fake.write_calls);
    EXPECT_TRUE(m_fake.tx.empty());
}

TEST_F(MasterTest, SubmitWhileArmedOrWaitingIsBusy) {
    Submit(0x01);
    EXPECT_EQ(CHIPS_SUBMIT_BUSY, chips_master_submit(&m_master, 0x02, nullptr, 0, K_TIMEOUT_US, m_rbuf));
    Step();
    ASSERT_EQ(CHIPS_MASTER_WAITING, State());
    EXPECT_EQ(CHIPS_SUBMIT_BUSY, chips_master_submit(&m_master, 0x02, nullptr, 0, K_TIMEOUT_US, m_rbuf));

    EXPECT_EQ(0x01u, Result().cmd);
    EXPECT_EQ(1u, Result().seq);
}

TEST_F(MasterTest, SubmitRejectsInvalidArguments) {
    const uint8_t one = 0;
    const Bytes too_big(CHIPS_MAX_PAYLOAD + 1, 0xAB);

    if (CHIPS_CMD_MASK < 0xFF) {
        EXPECT_EQ(CHIPS_SUBMIT_INVALID_ARG, chips_master_submit(&m_master, static_cast<uint8_t>(CHIPS_CMD_MASK + 1),
                                                                &one, 1, K_TIMEOUT_US, m_rbuf));
    }
    EXPECT_EQ(CHIPS_SUBMIT_INVALID_ARG,
              chips_master_submit(&m_master, 0x01, too_big.data(), too_big.size(), K_TIMEOUT_US, m_rbuf));
    EXPECT_EQ(CHIPS_SUBMIT_INVALID_ARG, chips_master_submit(&m_master, 0x01, nullptr, 1, K_TIMEOUT_US, m_rbuf));
    EXPECT_EQ(CHIPS_SUBMIT_INVALID_ARG, chips_master_submit(&m_master, 0x01, &one, 1, 0, m_rbuf));
    if (CHIPS_TIME_MAX_US < std::numeric_limits<uint32_t>::max()) {
        EXPECT_EQ(CHIPS_SUBMIT_INVALID_ARG,
                  chips_master_submit(&m_master, 0x01, &one, 1, CHIPS_TIME_MAX_US + 1, m_rbuf));
    }
    EXPECT_EQ(CHIPS_SUBMIT_INVALID_ARG, chips_master_submit(&m_master, 0x01, &one, 1, K_TIMEOUT_US, nullptr));

    EXPECT_EQ(CHIPS_MASTER_IDLE, State());
    EXPECT_EQ(CHIPS_OUTCOME_PENDING, Result().outcome);
    Submit(0x01);
    EXPECT_EQ(1u, Seq());
}

TEST_F(MasterTest, SubmitAcceptsBoundaryArguments) {
    const Bytes max_payload(CHIPS_MAX_PAYLOAD, 0x5A);
    EXPECT_EQ(CHIPS_SUBMIT_OK, chips_master_submit(&m_master, CHIPS_CMD_MASK, max_payload.data(), max_payload.size(),
                                                   CHIPS_TIME_MAX_US, m_rbuf));
    EXPECT_TRUE(chips_master_cancel(&m_master));

    EXPECT_EQ(CHIPS_SUBMIT_OK, chips_master_submit(&m_master, 0x00, nullptr, 0, 1, m_rbuf));
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
    EXPECT_EQ(0u, Result().latency_us);
    EXPECT_EQ(0x02u, Result().cmd);
}

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

    m_fake.now = K_RESEND_US - 1;
    EXPECT_FALSE(Has(Step(), CHIPS_EV_TX_FRAME));
    EXPECT_EQ(writes, m_fake.write_calls);
}

TEST_F(MasterTest, ResendsEveryPeriodMeasuredFromLastSend) {
    Submit(0x01, {9});
    Step();
    const Bytes frame = m_fake.tx;

    m_fake.now = K_RESEND_US;
    EXPECT_TRUE(Has(Step(), CHIPS_EV_TX_FRAME));

    m_fake.now = 2 * K_RESEND_US - 1;
    EXPECT_FALSE(Has(Step(), CHIPS_EV_TX_FRAME));

    m_fake.now = 2 * K_RESEND_US;
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
    m_fake.now = 3 * K_RESEND_US;
    Step();

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
    m_fake.now = K_RESEND_US - 1;
    Step();
    EXPECT_EQ(writes, m_fake.write_calls);

    m_fake.now = K_RESEND_US;
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
    m_fake.now = K_RESEND_US;
    EXPECT_TRUE(Has(Step(), CHIPS_EV_TX_FRAME));
    EXPECT_EQ(expected, m_fake.tx);
}

TEST_F(MasterTest, MatchingResponseCompletesWithPayloadAndLatency) {
    m_fake.now = 100;
    Submit(0x21, {7});
    Step();

    m_fake.now = 1334;
    m_fake.Feed(Response({9, 8, 7}));
    const uint32_t ev = Step();

    EXPECT_TRUE(Has(ev, CHIPS_EV_DONE));
    EXPECT_EQ(CHIPS_MASTER_IDLE, State());
    EXPECT_EQ(CHIPS_OUTCOME_OK, Result().outcome);
    EXPECT_EQ(1234u, Result().latency_us);
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
    m_fake.now += 500;
    m_fake.Feed(Response());
    Step();
    EXPECT_EQ(500u, Result().latency_us);

    Submit(0x01);
    Step();
    m_fake.now += 200;
    m_fake.Feed(Response());
    Step();
    EXPECT_EQ(200u, Result().latency_us);
}

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

TEST_F(MasterTest, CmdMismatchFinishesTransaction) {
    Submit(0x01);
    Step();
    m_fake.now = 77;
    m_fake.Feed(Resp(Seq(), 0x02, {1}));

    EXPECT_TRUE(Has(Step(), CHIPS_EV_DONE));
    EXPECT_EQ(CHIPS_OUTCOME_CMD_MISMATCH, Result().outcome);
    EXPECT_EQ(77u, Result().latency_us);
    EXPECT_EQ(nullptr, Result().payload);
    EXPECT_EQ(0u, Result().payload_len);
}

TEST_F(MasterTest, RequestFrameFinishesAsUnexpectedRequest) {
    Submit(0x01);
    Step();
    m_fake.Feed(Request());

    EXPECT_TRUE(Has(Step(), CHIPS_EV_DONE));
    EXPECT_EQ(CHIPS_OUTCOME_UNEXPECTED_REQUEST, Result().outcome);
}

TEST_F(MasterTest, UnexpectedRequestTakesPrecedenceOverStaleSeq) {
    Submit(0x01);
    Step();
    m_fake.Feed(Req(static_cast<uint8_t>(Seq() + 1), Cmd()));

    EXPECT_TRUE(Has(Step(), CHIPS_EV_DONE));
    EXPECT_EQ(CHIPS_OUTCOME_UNEXPECTED_REQUEST, Result().outcome);
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
    m_fake.Feed(Bytes(CHIPS_MASTER_RX_CHUNK + 10, 0x00));
    Step();
    EXPECT_EQ(10u, m_fake.rx.size());
}

TEST_F(MasterTest, ResponseQueuedBeforeFirstSendIsStale) {
    m_fake.now = 50;
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

    m_fake.now = K_TIMEOUT_US;
    EXPECT_TRUE(Has(Step(), CHIPS_EV_DONE));
    EXPECT_EQ(CHIPS_OUTCOME_TIMEOUT, Result().outcome);
}

TEST_F(MasterTest, TimesOutExactlyAtDeadline) {
    Submit(0x01);
    Step();

    m_fake.now = K_TIMEOUT_US - 1;
    EXPECT_FALSE(Has(Step(), CHIPS_EV_DONE));
    EXPECT_EQ(CHIPS_MASTER_WAITING, State());

    m_fake.now = K_TIMEOUT_US;
    EXPECT_TRUE(Has(Step(), CHIPS_EV_DONE));
    EXPECT_EQ(CHIPS_MASTER_IDLE, State());
    EXPECT_EQ(CHIPS_OUTCOME_TIMEOUT, Result().outcome);
    EXPECT_EQ(K_TIMEOUT_US, Result().latency_us);
}

TEST_F(MasterTest, TimeoutIsMeasuredFromFirstStepNotSubmit) {
    Submit(0x01);
    m_fake.now = 5000;
    Step();
    m_fake.now = 5000 + K_TIMEOUT_US - 1;
    EXPECT_FALSE(Has(Step(), CHIPS_EV_DONE));
    m_fake.now = 5000 + K_TIMEOUT_US;
    EXPECT_TRUE(Has(Step(), CHIPS_EV_DONE));
    EXPECT_EQ(CHIPS_OUTCOME_TIMEOUT, Result().outcome);
}

TEST_F(MasterTest, ResponseArrivingAtDeadlineWins) {
    Submit(0x01);
    Step();
    m_fake.now = K_TIMEOUT_US;
    m_fake.Feed(Response({5}));
    EXPECT_TRUE(Has(Step(), CHIPS_EV_DONE));
    EXPECT_EQ(CHIPS_OUTCOME_OK, Result().outcome);
}

TEST_F(MasterTest, NoTransmitOnTheStepThatTimesOut) {
    Submit(0x01);
    Step();
    const int writes = m_fake.write_calls;
    m_fake.now = K_TIMEOUT_US;
    EXPECT_FALSE(Has(Step(), CHIPS_EV_TX_FRAME));
    EXPECT_EQ(writes, m_fake.write_calls);
}

TEST_F(MasterTest, SendFailedWhenNoFrameEverWentOut) {
    m_fake.fail_write = true;
    Submit(0x01);
    StepUntil(CHIPS_EV_TX_ERROR);
    m_fake.now = K_TIMEOUT_US;
    EXPECT_TRUE(Has(Step(), CHIPS_EV_DONE));
    EXPECT_EQ(CHIPS_OUTCOME_SEND_FAILED, Result().outcome);
}

TEST_F(MasterTest, TimeoutAfterOneSuccessfulSendIsTimeoutEvenIfResendsFail) {
    Submit(0x01);
    Step();
    m_fake.fail_write = true;
    m_fake.now = K_RESEND_US;
    EXPECT_TRUE(Has(Step(), CHIPS_EV_TX_ERROR));
    m_fake.now = K_TIMEOUT_US;
    EXPECT_TRUE(Has(Step(), CHIPS_EV_DONE));
    EXPECT_EQ(CHIPS_OUTCOME_TIMEOUT, Result().outcome);
}

TEST_F(MasterTest, TimeoutWithFrameStuckInFlightAbortsTx) {
    m_fake.max_write = 0;
    Submit(0x01);
    Step();
    EXPECT_EQ(1, m_fake.write_calls);
    EXPECT_TRUE(m_fake.tx.empty());

    m_fake.now = K_TIMEOUT_US;
    EXPECT_TRUE(Has(Step(), CHIPS_EV_DONE));
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
    const uint32_t start = std::numeric_limits<uint32_t>::max() - 99;
    m_fake.now = start;
    Submit(0x01);
    Step();

    m_fake.now = static_cast<uint32_t>(start + K_TIMEOUT_US - 1);
    EXPECT_FALSE(Has(Step(), CHIPS_EV_DONE));
    m_fake.now = static_cast<uint32_t>(start + K_TIMEOUT_US);
    EXPECT_TRUE(Has(Step(), CHIPS_EV_DONE));
    EXPECT_EQ(CHIPS_OUTCOME_TIMEOUT, Result().outcome);
    EXPECT_EQ(K_TIMEOUT_US, Result().latency_us);
}

TEST_F(MasterTest, LatencyWorksAcrossClockWraparound) {
    m_fake.now = std::numeric_limits<uint32_t>::max() - 99;
    Submit(0x01);
    Step();
    m_fake.now = 100;
    m_fake.Feed(Response());
    EXPECT_TRUE(Has(Step(), CHIPS_EV_DONE));
    EXPECT_EQ(200u, Result().latency_us);
}

TEST_F(MasterTest, CancelWhenIdleReturnsFalse) {
    EXPECT_FALSE(chips_master_cancel(&m_master));
    EXPECT_EQ(CHIPS_OUTCOME_PENDING, Result().outcome);
}

TEST_F(MasterTest, CancelArmedTransactionNeverTransmits) {
    Submit(0x01);
    EXPECT_TRUE(chips_master_cancel(&m_master));
    EXPECT_EQ(CHIPS_MASTER_IDLE, State());
    EXPECT_EQ(CHIPS_OUTCOME_CANCELLED, Result().outcome);
    EXPECT_EQ(0u, Result().latency_us);

    Step();
    EXPECT_EQ(0, m_fake.write_calls);
    EXPECT_FALSE(chips_master_cancel(&m_master));
}

TEST_F(MasterTest, CancelWaitingTransaction) {
    Submit(0x01);
    Step();
    m_fake.now = 500;
    EXPECT_TRUE(chips_master_cancel(&m_master));
    EXPECT_EQ(CHIPS_OUTCOME_CANCELLED, Result().outcome);
    EXPECT_EQ(0u, Result().latency_us);
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
