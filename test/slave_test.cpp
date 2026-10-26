/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 EPFL Spacecraft Team
 */

#include "chips/slave.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>

#include "test_util.hpp"

namespace {

using namespace chips_test;

constexpr uint32_t K_WINDOW_US = 5000;

class SlaveTest : public ::testing::Test {
  protected:
    void SetUp() override {
        m_port = m_fake.Port();
        m_cfg = chips_slave_config_t{};
        m_cfg.cache_window_us = K_WINDOW_US;
        chips_slave_init(&m_slave, &m_port, &m_cfg);
    }

    uint32_t Step() {
        const uint32_t ev = chips_slave_step(&m_slave);
        m_tally.Add(ev);
        return ev;
    }

    uint32_t StepUntil(uint32_t flag, int max_steps = 1000) {
        return chips_test::StepUntil([this] { return Step(); }, flag, max_steps);
    }

    uint32_t Drain(int max_steps = 100) {
        return chips_test::Drain([this] { return Step(); }, m_fake, max_steps);
    }

    unsigned long Count(uint32_t flag) const { return m_tally.Count(flag); }

    chips_respond_status_t RespondRaw(const Bytes& payload) {
        return chips_slave_respond(&m_slave, payload.empty() ? nullptr : payload.data(), payload.size());
    }

    void Respond(const Bytes& payload = {}) { ASSERT_EQ(CHIPS_RESPOND_OK, RespondRaw(payload)); }

    void Deliver(const Bytes& req) {
        m_fake.Feed(req);
        ASSERT_TRUE(Has(StepUntil(CHIPS_SLAVE_EV_REQUEST, 100), CHIPS_SLAVE_EV_REQUEST));
        ASSERT_EQ(CHIPS_SLAVE_PENDING, State());
    }

    Bytes Serve(const Bytes& req, const Bytes& payload = {}) {
        Deliver(req);
        const uint8_t seq = Request()->seq;
        const uint8_t cmd = Request()->cmd;
        Respond(payload);
        EXPECT_TRUE(Has(StepUntil(CHIPS_SLAVE_EV_TX_FRAME), CHIPS_SLAVE_EV_TX_FRAME));
        const Bytes frame = Resp(seq, cmd, payload);
        EXPECT_EQ(frame, m_fake.tx);
        m_fake.tx.clear();
        return frame;
    }

    const chips_slave_request_t* Request() const { return chips_slave_request(&m_slave); }
    chips_slave_state_t State() const { return chips_slave_state(&m_slave); }

    Bytes RequestPayload() const {
        const chips_slave_request_t* r = Request();
        if (r == nullptr || r->payload_len == 0) {
            return {};
        }
        return Bytes(r->payload, r->payload + r->payload_len);
    }

    int PendingHalf() const {
        const chips_slave_request_t* r = Request();
        if (r == nullptr) {
            return -1;
        }
        for (int i = 0; i < 2; ++i) {
            if (r->payload == m_slave.rx_payload[i]) {
                return i;
            }
        }
        return -1;
    }

    FakePort m_fake;
    chips_port_t m_port{};
    chips_slave_config_t m_cfg{};
    chips_slave_t m_slave;
    EventTally m_tally;
};

TEST_F(SlaveTest, InitStartsListeningWithNoRequest) {
    EXPECT_EQ(CHIPS_SLAVE_LISTENING, State());
    EXPECT_EQ(nullptr, Request());
    EXPECT_EQ(0, m_fake.read_calls);
    EXPECT_EQ(0, m_fake.write_calls);
}

TEST_F(SlaveTest, IdleStepWithNothingToReadIsQuiet) {
    EXPECT_EQ(static_cast<uint32_t>(CHIPS_SLAVE_EV_NONE), Step());
    EXPECT_EQ(1, m_fake.read_calls);
    EXPECT_EQ(0, m_fake.write_calls);
}

TEST_F(SlaveTest, EveryStepReadsOnce) {
    Step();
    Deliver(Req(1, 0x01));
    Respond();
    Step();
    Step();
    EXPECT_EQ(4, m_fake.read_calls);
}

TEST_F(SlaveTest, ReadsAtMostOneChunkPerStep) {
    const Bytes junk(CHIPS_SLAVE_RX_CHUNK + 10, 0x00);
    m_fake.Feed(junk);
    Step();
    EXPECT_EQ(10u, m_fake.rx.size());
    Step();
    EXPECT_TRUE(m_fake.rx.empty());
}

TEST_F(SlaveTest, RequestIsDelivered) {
    m_fake.Feed(Req(7, 0x12, {1, 2, 3}));
    const uint32_t ev = Step();

    EXPECT_TRUE(Has(ev, CHIPS_SLAVE_EV_REQUEST));
    EXPECT_EQ(CHIPS_SLAVE_PENDING, State());
    ASSERT_NE(nullptr, Request());
    EXPECT_EQ(7u, Request()->seq);
    EXPECT_EQ(0x12u, Request()->cmd);
    EXPECT_EQ((Bytes{1, 2, 3}), RequestPayload());
    EXPECT_TRUE(m_fake.tx.empty());
}

TEST_F(SlaveTest, EmptyPayloadRequest) {
    Deliver(Req(1, 0x01));
    EXPECT_EQ(0u, Request()->payload_len);
}

TEST_F(SlaveTest, MaxPayloadRequest) {
    const Bytes big = Pattern(CHIPS_MAX_PAYLOAD);
    m_fake.Feed(Req(1, 0x01, big));
    EXPECT_TRUE(Has(StepUntil(CHIPS_SLAVE_EV_REQUEST), CHIPS_SLAVE_EV_REQUEST));
    EXPECT_EQ(big, RequestPayload());
}

TEST_F(SlaveTest, RequestSplitAcrossManyReads) {
    m_fake.max_read = 1;
    const Bytes req = Req(3, 0x04, {5, 6, 7, 8});
    m_fake.Feed(req);

    EXPECT_TRUE(Has(StepUntil(CHIPS_SLAVE_EV_REQUEST), CHIPS_SLAVE_EV_REQUEST));
    EXPECT_EQ(req.size(), m_tally.steps);
    EXPECT_TRUE(m_fake.rx.empty());
    EXPECT_EQ((Bytes{5, 6, 7, 8}), RequestPayload());
}

TEST_F(SlaveTest, ResponseFramesAreIgnored) {
    m_fake.Feed(Resp(1, 0x01, {9}));
    const uint32_t ev = Step();
    EXPECT_TRUE(Has(ev, CHIPS_SLAVE_EV_RX_IGNORED));
    EXPECT_FALSE(Has(ev, CHIPS_SLAVE_EV_REQUEST));
    EXPECT_EQ(CHIPS_SLAVE_LISTENING, State());
}

TEST_F(SlaveTest, AnySeqIsAcceptedInAnyOrder) {
    Serve(Req(200, 0x01));
    Serve(Req(3, 0x01));
    Serve(Req(3, 0x02));
    EXPECT_EQ(3u, Count(CHIPS_SLAVE_EV_REQUEST));
    EXPECT_EQ(3u, Count(CHIPS_SLAVE_EV_TX_FRAME));
}

TEST_F(SlaveTest, RequestPayloadPointsIntoReceiveBuffer) {
    Deliver(Req(1, 0x01, {1, 2, 3}));
    EXPECT_NE(-1, PendingHalf());
}

TEST_F(SlaveTest, ConsecutiveRequestsAlternateHalves) {
    Deliver(Req(1, 0x01, {1}));
    const int first = PendingHalf();
    ASSERT_NE(-1, first);
    Respond();

    Deliver(Req(2, 0x01, {2}));
    EXPECT_EQ(1 - first, PendingHalf());
    ASSERT_EQ(CHIPS_RESPOND_OK, chips_slave_abandon(&m_slave));

    Deliver(Req(3, 0x01, {3}));
    EXPECT_EQ(first, PendingHalf());
    EXPECT_EQ((Bytes{3}), RequestPayload());
}

TEST_F(SlaveTest, PendingRequestSurvivesLaterFrames) {
    const Bytes pending = Pattern(CHIPS_MAX_PAYLOAD, 37u, 1u);
    m_fake.Feed(Req(1, 0x01, pending));
    ASSERT_TRUE(Has(StepUntil(CHIPS_SLAVE_EV_REQUEST), CHIPS_SLAVE_EV_REQUEST));

    m_fake.Feed(Req(2, 0x01, Bytes(CHIPS_MAX_PAYLOAD, 0xAA)));
    m_fake.Feed(Resp(5, 0x05, Bytes(CHIPS_MAX_PAYLOAD, 0xBB)));
    Bytes bad = Req(6, 0x01, Bytes(CHIPS_MAX_PAYLOAD, 0xCC));
    bad[bad.size() / 2] ^= 0x01;
    m_fake.Feed(bad);
    const uint32_t ev = Drain(1000);

    EXPECT_TRUE(Has(ev, CHIPS_SLAVE_EV_RX_IGNORED));
    EXPECT_TRUE(Has(ev, CHIPS_SLAVE_EV_RX_BAD_FRAME));
    ASSERT_NE(nullptr, Request());
    EXPECT_EQ(1u, Request()->seq);
    EXPECT_EQ(pending, RequestPayload());
}

TEST_F(SlaveTest, RespondMayEchoRequestPayloadInPlace) {
    const Bytes payload = Pattern(300);
    Deliver(Req(4, 0x07, payload));
    const chips_slave_request_t* r = Request();
    ASSERT_EQ(CHIPS_RESPOND_OK, chips_slave_respond(&m_slave, r->payload, r->payload_len));

    EXPECT_TRUE(Has(StepUntil(CHIPS_SLAVE_EV_TX_FRAME), CHIPS_SLAVE_EV_TX_FRAME));
    EXPECT_EQ(Resp(4, 0x07, payload), m_fake.tx);
}

TEST_F(SlaveTest, FrameSharingDelimiterWithDeliveredRequestIsReceived) {
    const Bytes a = Req(1, 0x01, {1, 2});
    const Bytes b = Req(2, 0x01, {3, 4});
    ASSERT_EQ(CHIPS_SYNC, b.front());

    Deliver(a);
    Respond();
    m_fake.Feed(Bytes(b.begin() + 1, b.end()));
    ASSERT_TRUE(Has(Step(), CHIPS_SLAVE_EV_REQUEST));
    ASSERT_NE(nullptr, Request());
    EXPECT_EQ(2u, Request()->seq);
    EXPECT_EQ((Bytes{3, 4}), RequestPayload());
}

TEST_F(SlaveTest, DuplicateWhilePendingIsIgnored) {
    const Bytes req = Req(1, 0x01, {4});
    Deliver(req);
    m_fake.Feed(req);

    const uint32_t ev = Step();
    EXPECT_TRUE(Has(ev, CHIPS_SLAVE_EV_RX_IGNORED));
    EXPECT_FALSE(Has(ev, CHIPS_SLAVE_EV_REQUEST));
    EXPECT_EQ(1u, Count(CHIPS_SLAVE_EV_REQUEST));
    EXPECT_EQ((Bytes{4}), RequestPayload());
}

TEST_F(SlaveTest, DifferentRequestWhilePendingIsIgnored) {
    Deliver(Req(1, 0x01));
    m_fake.Feed(Req(2, 0x01));

    EXPECT_TRUE(Has(Step(), CHIPS_SLAVE_EV_RX_IGNORED));
    EXPECT_EQ(1u, Request()->seq);
}

TEST_F(SlaveTest, SamePayloadDifferentCmdWhilePendingIsIgnored) {
    Deliver(Req(1, 0x01, {7}));
    m_fake.Feed(Req(1, 0x02, {7}));
    EXPECT_TRUE(Has(Step(), CHIPS_SLAVE_EV_RX_IGNORED));
    EXPECT_EQ(0x01u, Request()->cmd);
}

TEST_F(SlaveTest, RequestAndDuplicateBackToBack) {
    const Bytes req = Req(1, 0x01);
    m_fake.Feed(req);
    m_fake.Feed(req);

    const uint32_t ev = Drain();
    EXPECT_TRUE(Has(ev, CHIPS_SLAVE_EV_REQUEST));
    EXPECT_TRUE(Has(ev, CHIPS_SLAVE_EV_RX_IGNORED));
    EXPECT_EQ(1u, Count(CHIPS_SLAVE_EV_REQUEST));
}

TEST_F(SlaveTest, RespondWithoutRequestIsRejected) {
    EXPECT_EQ(CHIPS_RESPOND_NO_REQUEST, RespondRaw({1}));
    Step();
    EXPECT_EQ(0, m_fake.write_calls);
}

TEST_F(SlaveTest, RespondRejectsInvalidArgumentsAndKeepsRequest) {
    Deliver(Req(1, 0x01, {3}));
    const Bytes too_big(CHIPS_MAX_PAYLOAD + 1, 0xAB);

    EXPECT_EQ(CHIPS_RESPOND_INVALID_ARG, RespondRaw(too_big));
    EXPECT_EQ(CHIPS_RESPOND_INVALID_ARG, chips_slave_respond(&m_slave, nullptr, 1));

    EXPECT_EQ(CHIPS_SLAVE_PENDING, State());
    ASSERT_NE(nullptr, Request());
    EXPECT_EQ((Bytes{3}), RequestPayload());
    Step();
    EXPECT_EQ(0, m_fake.write_calls);

    Respond({4});
}

TEST_F(SlaveTest, RespondAcceptsBoundaryPayloads) {
    Deliver(Req(1, 0x01));
    EXPECT_EQ(CHIPS_RESPOND_OK, chips_slave_respond(&m_slave, nullptr, 0));

    Deliver(Req(2, 0x01));
    Respond(Bytes(CHIPS_MAX_PAYLOAD, 0x5A));
    EXPECT_TRUE(Has(StepUntil(CHIPS_SLAVE_EV_TX_FRAME), CHIPS_SLAVE_EV_TX_FRAME));
}

TEST_F(SlaveTest, RespondReturnsToListeningAndSendsOnNextStep) {
    Deliver(Req(9, 0x33, {1}));
    Respond({0xC0, 0xFF, 0xEE});

    EXPECT_EQ(CHIPS_SLAVE_LISTENING, State());
    EXPECT_EQ(nullptr, Request());
    EXPECT_EQ(0, m_fake.write_calls);
    EXPECT_EQ(CHIPS_RESPOND_NO_REQUEST, RespondRaw({}));

    EXPECT_TRUE(Has(Step(), CHIPS_SLAVE_EV_TX_FRAME));
    EXPECT_EQ(Resp(9, 0x33, {0xC0, 0xFF, 0xEE}), m_fake.tx);
    EXPECT_EQ(1u, Count(CHIPS_SLAVE_EV_TX_FRAME));

    const int writes = m_fake.write_calls;
    Step();
    EXPECT_EQ(writes, m_fake.write_calls);
}

TEST_F(SlaveTest, ShortWritesAreContinuedOnLaterSteps) {
    m_fake.max_write = 3;
    Deliver(Req(1, 0x01));
    Respond({1, 2, 3, 4, 5, 6, 7, 8});
    const Bytes expected = Resp(1, 0x01, {1, 2, 3, 4, 5, 6, 7, 8});
    ASSERT_GT(expected.size(), 3u);

    EXPECT_FALSE(Has(Step(), CHIPS_SLAVE_EV_TX_FRAME));
    EXPECT_EQ(3u, m_fake.tx.size());

    EXPECT_TRUE(Has(StepUntil(CHIPS_SLAVE_EV_TX_FRAME, 100), CHIPS_SLAVE_EV_TX_FRAME));
    EXPECT_EQ(expected, m_fake.tx);
    EXPECT_EQ(1u, Count(CHIPS_SLAVE_EV_TX_FRAME));
    EXPECT_EQ(static_cast<int>((expected.size() + 2) / 3), m_fake.write_calls);
}

TEST_F(SlaveTest, ZeroByteWritesStallWithoutLosingTheFrame) {
    m_fake.max_write = 0;
    Deliver(Req(1, 0x01, {1}));
    Respond({2});
    for (int i = 0; i < 5; ++i) {
        EXPECT_FALSE(Has(Step(), CHIPS_SLAVE_EV_TX_FRAME));
    }
    EXPECT_EQ(5, m_fake.write_calls);
    EXPECT_TRUE(m_fake.tx.empty());

    m_fake.max_write = std::numeric_limits<size_t>::max();
    EXPECT_TRUE(Has(Step(), CHIPS_SLAVE_EV_TX_FRAME));
    EXPECT_EQ(Resp(1, 0x01, {2}), m_fake.tx);
}

TEST_F(SlaveTest, WriteErrorDropsTheFrame) {
    m_fake.fail_write = true;
    Deliver(Req(1, 0x01));
    Respond({1});

    const uint32_t ev = Step();
    EXPECT_TRUE(Has(ev, CHIPS_SLAVE_EV_TX_ERROR));
    EXPECT_FALSE(Has(ev, CHIPS_SLAVE_EV_TX_FRAME));
    EXPECT_TRUE(m_fake.tx.empty());

    m_fake.fail_write = false;
    const int writes = m_fake.write_calls;
    Step();
    EXPECT_EQ(writes, m_fake.write_calls);
}

TEST_F(SlaveTest, WriteErrorIsRecoveredByMasterResend) {
    const Bytes req = Req(1, 0x01, {6});
    m_fake.fail_write = true;
    Deliver(req);
    Respond({7});
    EXPECT_TRUE(Has(Step(), CHIPS_SLAVE_EV_TX_ERROR));

    m_fake.fail_write = false;
    m_fake.Feed(req);
    const uint32_t ev = Step();
    EXPECT_TRUE(Has(ev, CHIPS_SLAVE_EV_RETRANSMIT));
    EXPECT_TRUE(Has(ev, CHIPS_SLAVE_EV_TX_FRAME));
    EXPECT_EQ(Resp(1, 0x01, {7}), m_fake.tx);
}

TEST_F(SlaveTest, ReceiveAndTransmitHappenInTheSameStep) {
    Deliver(Req(1, 0x01));
    Respond();
    m_fake.Feed(Req(2, 0x01));

    const uint32_t ev = Step();
    EXPECT_TRUE(Has(ev, CHIPS_SLAVE_EV_REQUEST));
    EXPECT_TRUE(Has(ev, CHIPS_SLAVE_EV_TX_FRAME));
    EXPECT_EQ(Resp(1, 0x01), m_fake.tx);
    EXPECT_EQ(2u, Request()->seq);
}

TEST_F(SlaveTest, RespondingAbortsPreviousResponseStillInFlight) {
    m_fake.max_write = 2;
    Deliver(Req(1, 0x01));
    Respond({1, 2, 3, 4, 5, 6, 7, 8});
    m_fake.Feed(Req(2, 0x02));
    const uint32_t ev = Step();
    ASSERT_EQ(CHIPS_SLAVE_PENDING, State());
    ASSERT_FALSE(Has(ev, CHIPS_SLAVE_EV_TX_FRAME));
    ASSERT_EQ(2u, m_fake.tx.size());

    Respond({9});

    m_fake.max_write = std::numeric_limits<size_t>::max();
    m_fake.tx.clear();
    EXPECT_TRUE(Has(Step(), CHIPS_SLAVE_EV_TX_FRAME));
    EXPECT_EQ(Resp(2, 0x02, {9}), m_fake.tx);
}

TEST_F(SlaveTest, DuplicateWithinWindowRetransmitsCachedResponse) {
    const Bytes req = Req(4, 0x10, {1, 2});
    const Bytes frame = Serve(req, {3, 4});

    m_fake.now = K_WINDOW_US - 1;
    m_fake.Feed(req);
    const uint32_t ev = Step();

    EXPECT_TRUE(Has(ev, CHIPS_SLAVE_EV_RETRANSMIT));
    EXPECT_TRUE(Has(ev, CHIPS_SLAVE_EV_TX_FRAME));
    EXPECT_FALSE(Has(ev, CHIPS_SLAVE_EV_REQUEST));
    EXPECT_EQ(CHIPS_SLAVE_LISTENING, State());
    EXPECT_EQ(frame, m_fake.tx);
    EXPECT_EQ(1u, Count(CHIPS_SLAVE_EV_REQUEST));
    EXPECT_EQ(1u, Count(CHIPS_SLAVE_EV_RETRANSMIT));
}

TEST_F(SlaveTest, EveryDuplicateWithinWindowIsRetransmitted) {
    const Bytes req = Req(4, 0x10);
    const Bytes frame = Serve(req);

    for (int i = 0; i < 3; ++i) {
        m_fake.now += 100;
        m_fake.Feed(req);
        EXPECT_TRUE(Has(Step(), CHIPS_SLAVE_EV_RETRANSMIT));
    }
    EXPECT_EQ(3u, Count(CHIPS_SLAVE_EV_RETRANSMIT));
    EXPECT_EQ(Repeat(frame, 3), m_fake.tx);
}

TEST_F(SlaveTest, DuplicateAtWindowBoundaryIsDeliveredAgain) {
    const Bytes req = Req(4, 0x10, {1});
    m_fake.now = 500;
    Serve(req);

    m_fake.now = 500 + K_WINDOW_US;
    m_fake.Feed(req);
    const uint32_t ev = Step();

    EXPECT_TRUE(Has(ev, CHIPS_SLAVE_EV_REQUEST));
    EXPECT_FALSE(Has(ev, CHIPS_SLAVE_EV_RETRANSMIT));
    EXPECT_EQ(CHIPS_SLAVE_PENDING, State());
    EXPECT_EQ(2u, Count(CHIPS_SLAVE_EV_REQUEST));
    EXPECT_TRUE(m_fake.tx.empty());
}

TEST_F(SlaveTest, WindowIsMeasuredFromRespondCall) {
    const Bytes req = Req(4, 0x10);
    m_fake.now = 0;
    Deliver(req);
    m_fake.now = 2000;
    Respond();
    Step();
    m_fake.tx.clear();

    m_fake.now = 2000 + K_WINDOW_US - 1;
    m_fake.Feed(req);
    EXPECT_TRUE(Has(Step(), CHIPS_SLAVE_EV_RETRANSMIT));
}

TEST_F(SlaveTest, ExpiredCacheIsNotUsedAgain) {
    const Bytes req = Req(4, 0x10);
    Serve(req);
    m_fake.now = K_WINDOW_US;
    Deliver(req);
    ASSERT_EQ(CHIPS_RESPOND_OK, chips_slave_abandon(&m_slave));

    m_fake.now += 1;
    m_fake.Feed(req);
    const uint32_t ev = Step();
    EXPECT_TRUE(Has(ev, CHIPS_SLAVE_EV_REQUEST));
    EXPECT_FALSE(Has(ev, CHIPS_SLAVE_EV_RETRANSMIT));
}

TEST_F(SlaveTest, CacheWindowWorksAcrossClockWraparound) {
    const Bytes req = Req(4, 0x10);
    const uint32_t start = std::numeric_limits<uint32_t>::max() - 99;
    m_fake.now = start;
    Serve(req);

    m_fake.now = static_cast<uint32_t>(start + K_WINDOW_US - 1);
    m_fake.Feed(req);
    EXPECT_TRUE(Has(Step(), CHIPS_SLAVE_EV_RETRANSMIT));

    m_fake.now = static_cast<uint32_t>(start + K_WINDOW_US);
    m_fake.Feed(req);
    EXPECT_TRUE(Has(Step(), CHIPS_SLAVE_EV_REQUEST));
}

TEST_F(SlaveTest, DuplicateWhileResponseInFlightIsIgnored) {
    m_fake.max_write = 2;
    const Bytes req = Req(1, 0x01);
    Deliver(req);
    Respond({1, 2, 3, 4, 5, 6});
    const Bytes expected = Resp(1, 0x01, {1, 2, 3, 4, 5, 6});
    ASSERT_FALSE(Has(Step(), CHIPS_SLAVE_EV_TX_FRAME));

    m_fake.Feed(req);
    const uint32_t ev = Step();
    EXPECT_TRUE(Has(ev, CHIPS_SLAVE_EV_RX_IGNORED));
    EXPECT_FALSE(Has(ev, CHIPS_SLAVE_EV_RETRANSMIT));

    StepUntil(CHIPS_SLAVE_EV_TX_FRAME, 100);
    EXPECT_EQ(expected, m_fake.tx);
    EXPECT_EQ(0u, Count(CHIPS_SLAVE_EV_RETRANSMIT));
}

TEST_F(SlaveTest, SameSeqAndCmdWithDifferentPayloadIsANewRequest) {
    Serve(Req(4, 0x10, {1}));
    m_fake.Feed(Req(4, 0x10, {2}));
    const uint32_t ev = Step();
    EXPECT_TRUE(Has(ev, CHIPS_SLAVE_EV_REQUEST));
    EXPECT_FALSE(Has(ev, CHIPS_SLAVE_EV_RETRANSMIT));
    EXPECT_EQ((Bytes{2}), RequestPayload());
}

TEST_F(SlaveTest, SameSeqDifferentCmdIsANewRequest) {
    Serve(Req(4, 0x10));
    m_fake.Feed(Req(4, 0x11));
    EXPECT_TRUE(Has(Step(), CHIPS_SLAVE_EV_REQUEST));
    EXPECT_EQ(0x11u, Request()->cmd);
}

TEST_F(SlaveTest, DifferentSeqSameCmdAndPayloadIsANewRequest) {
    Serve(Req(4, 0x10, {1}));
    m_fake.Feed(Req(5, 0x10, {1}));
    EXPECT_TRUE(Has(Step(), CHIPS_SLAVE_EV_REQUEST));
    EXPECT_EQ(5u, Request()->seq);
}

TEST_F(SlaveTest, OnlyTheLatestResponseIsCached) {
    const Bytes a = Req(1, 0x01);
    Serve(a);
    Serve(Req(2, 0x01));

    m_fake.Feed(a);
    EXPECT_TRUE(Has(Step(), CHIPS_SLAVE_EV_REQUEST));
}

TEST_F(SlaveTest, DuplicateOfCachedRequestWhileAnotherIsPendingIsIgnored) {
    const Bytes a = Req(1, 0x01);
    Serve(a);
    Deliver(Req(2, 0x01));

    m_fake.Feed(a);
    const uint32_t ev = Step();
    EXPECT_TRUE(Has(ev, CHIPS_SLAVE_EV_RX_IGNORED));
    EXPECT_FALSE(Has(ev, CHIPS_SLAVE_EV_RETRANSMIT));
    EXPECT_TRUE(m_fake.tx.empty());
}

TEST_F(SlaveTest, InvalidateCacheMakesDuplicateADeliveredRequest) {
    const Bytes req = Req(1, 0x01);
    Serve(req);
    chips_slave_invalidate_cache(&m_slave);

    m_fake.Feed(req);
    const uint32_t ev = Step();
    EXPECT_TRUE(Has(ev, CHIPS_SLAVE_EV_REQUEST));
    EXPECT_FALSE(Has(ev, CHIPS_SLAVE_EV_RETRANSMIT));
}

TEST_F(SlaveTest, InvalidateCacheAbortsResponseInFlight) {
    m_fake.max_write = 2;
    Deliver(Req(1, 0x01));
    Respond({1, 2, 3, 4});
    Step();

    chips_slave_invalidate_cache(&m_slave);
    const int writes = m_fake.write_calls;
    Step();
    EXPECT_EQ(writes, m_fake.write_calls);
    EXPECT_EQ(2u, m_fake.tx.size());
}

TEST_F(SlaveTest, InvalidateCacheKeepsPendingRequest) {
    Deliver(Req(1, 0x01, {5}));
    chips_slave_invalidate_cache(&m_slave);
    EXPECT_EQ(CHIPS_SLAVE_PENDING, State());
    EXPECT_EQ((Bytes{5}), RequestPayload());
}

TEST_F(SlaveTest, AbandonWithoutRequestIsRejected) {
    EXPECT_EQ(CHIPS_RESPOND_NO_REQUEST, chips_slave_abandon(&m_slave));
}

TEST_F(SlaveTest, AbandonReturnsToListeningWithoutResponding) {
    Deliver(Req(1, 0x01));
    EXPECT_EQ(CHIPS_RESPOND_OK, chips_slave_abandon(&m_slave));

    EXPECT_EQ(CHIPS_SLAVE_LISTENING, State());
    EXPECT_EQ(nullptr, Request());
    EXPECT_EQ(CHIPS_RESPOND_NO_REQUEST, RespondRaw({}));
    Step();
    EXPECT_EQ(0, m_fake.write_calls);
}

TEST_F(SlaveTest, RetryOfAbandonedRequestIsDeliveredAgain) {
    const Bytes req = Req(1, 0x01, {8});
    Deliver(req);
    chips_slave_abandon(&m_slave);

    m_fake.Feed(req);
    EXPECT_TRUE(Has(Step(), CHIPS_SLAVE_EV_REQUEST));
    EXPECT_EQ(2u, Count(CHIPS_SLAVE_EV_REQUEST));
    EXPECT_EQ((Bytes{8}), RequestPayload());
}

TEST_F(SlaveTest, AbandonKeepsPreviousCacheAndInFlightResponse) {
    m_fake.max_write = 2;
    const Bytes a = Req(1, 0x01);
    Deliver(a);
    Respond({1, 2, 3, 4});
    const Bytes frame_a = Resp(1, 0x01, {1, 2, 3, 4});
    m_fake.Feed(Req(2, 0x01));
    Step();
    ASSERT_EQ(CHIPS_SLAVE_PENDING, State());

    chips_slave_abandon(&m_slave);
    StepUntil(CHIPS_SLAVE_EV_TX_FRAME, 100);
    EXPECT_EQ(frame_a, m_fake.tx);

    m_fake.max_write = std::numeric_limits<size_t>::max();
    m_fake.tx.clear();
    m_fake.Feed(a);
    EXPECT_TRUE(Has(Step(), CHIPS_SLAVE_EV_RETRANSMIT));
    EXPECT_EQ(frame_a, m_fake.tx);
}

TEST_F(SlaveTest, ReadErrorIsReported) {
    m_fake.fail_read = true;
    EXPECT_TRUE(Has(Step(), CHIPS_SLAVE_EV_RX_ERROR));
    EXPECT_EQ(CHIPS_SLAVE_LISTENING, State());
}

TEST_F(SlaveTest, ReadErrorDoesNotBlockTransmit) {
    Deliver(Req(1, 0x01));
    Respond();
    m_fake.fail_read = true;
    const uint32_t ev = Step();
    EXPECT_TRUE(Has(ev, CHIPS_SLAVE_EV_RX_ERROR));
    EXPECT_TRUE(Has(ev, CHIPS_SLAVE_EV_TX_FRAME));
}

TEST_F(SlaveTest, ReadErrorDiscardsPartiallyReceivedFrame) {
    const Bytes req = Req(1, 0x01, {1, 2, 3, 4, 5, 6});
    const size_t half = req.size() / 2;

    m_fake.Feed(Bytes(req.begin(), req.begin() + half));
    Step();
    m_fake.fail_read = true;
    Step();
    m_fake.fail_read = false;
    m_fake.Feed(Bytes(req.begin() + half, req.end()));

    EXPECT_FALSE(Has(Step(), CHIPS_SLAVE_EV_REQUEST));
    EXPECT_EQ(CHIPS_SLAVE_LISTENING, State());

    m_fake.Feed(req);
    EXPECT_TRUE(Has(StepUntil(CHIPS_SLAVE_EV_REQUEST, 5), CHIPS_SLAVE_EV_REQUEST));
    EXPECT_EQ((Bytes{1, 2, 3, 4, 5, 6}), RequestPayload());
}

TEST_F(SlaveTest, CorruptedFrameIsReportedAndNotDelivered) {
    Bytes bad = Req(1, 0x01, {0x41, 0x42, 0x43});
    ASSERT_TRUE(ReplaceFirst(bad, 0x42, 0x44)) << "payload byte not found verbatim in encoded frame";
    m_fake.Feed(bad);

    const uint32_t ev = Step();
    EXPECT_TRUE(Has(ev, CHIPS_SLAVE_EV_RX_BAD_FRAME));
    EXPECT_FALSE(Has(ev, CHIPS_SLAVE_EV_REQUEST));
    EXPECT_FALSE(Has(ev, CHIPS_SLAVE_EV_RX_IGNORED));
    EXPECT_EQ(CHIPS_SLAVE_LISTENING, State());

    m_fake.Feed(Req(1, 0x01, {0x41, 0x42, 0x43}));
    EXPECT_TRUE(Has(Step(), CHIPS_SLAVE_EV_REQUEST));
}

TEST_F(SlaveTest, CorruptedFrameDoesNotTriggerRetransmit) {
    const Bytes req = Req(1, 0x01, {0x41, 0x42, 0x43});
    Serve(req);
    Bytes bad = req;
    ASSERT_TRUE(ReplaceFirst(bad, 0x42, 0x44));
    m_fake.Feed(bad);
    const uint32_t ev = Step();
    EXPECT_TRUE(Has(ev, CHIPS_SLAVE_EV_RX_BAD_FRAME));
    EXPECT_FALSE(Has(ev, CHIPS_SLAVE_EV_RETRANSMIT));
    EXPECT_TRUE(m_fake.tx.empty());
}

TEST_F(SlaveTest, BackToBackExchanges) {
    for (uint8_t seq = 1; seq <= 5; ++seq) {
        Deliver(Req(seq, 0x20, {seq}));
        EXPECT_EQ((Bytes{seq}), RequestPayload());
        Respond({static_cast<uint8_t>(seq * 3)});
        ASSERT_TRUE(Has(Step(), CHIPS_SLAVE_EV_TX_FRAME));
        EXPECT_EQ(Resp(seq, 0x20, {static_cast<uint8_t>(seq * 3)}), m_fake.tx);
        m_fake.tx.clear();
    }
    EXPECT_EQ(5u, Count(CHIPS_SLAVE_EV_REQUEST));
    EXPECT_EQ(5u, Count(CHIPS_SLAVE_EV_TX_FRAME));
    EXPECT_EQ(0u, Count(CHIPS_SLAVE_EV_RETRANSMIT));
}

}  // namespace
