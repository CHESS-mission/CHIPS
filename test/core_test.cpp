#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <random>
#include <vector>

#include "chips/core.h"

namespace {

constexpr uint8_t k_cmd = static_cast<uint8_t>(0x05u & CHIPS_CMD_MASK);

class CoreTest : public ::testing::Test {
  protected:
    void SetUp() override {
        std::memset(&m_dec, 0, sizeof(m_dec));
        std::memset(m_payload_buf, 0, sizeof(m_payload_buf));
        chips_decoder_init(&m_dec, m_payload_buf);
    }

    static std::vector<uint8_t> Encode(uint8_t seq,
                                       chips_direction_t dir,
                                       uint8_t cmd,
                                       const std::vector<uint8_t>& payload) {
        std::vector<uint8_t> out(CHIPS_WIRE_MAX);
        const size_t n =
            chips_encode(seq, dir, cmd, payload.empty() ? nullptr : payload.data(), payload.size(), out.data());
        out.resize(n);
        return out;
    }

    chips_dec_status_t DecodeOnce(const std::vector<uint8_t>& wire, size_t* consumed) {
        return chips_decode(&m_dec, wire.data(), wire.size(), consumed, &m_frame);
    }

    chips_dec_status_t DecodeByteByByte(const std::vector<uint8_t>& wire) {
        for (const uint8_t b : wire) {
            size_t consumed = 0;
            const chips_dec_status_t st = chips_decode(&m_dec, &b, 1u, &consumed, &m_frame);
            EXPECT_LE(consumed, 1u);
            if (st != CHIPS_DEC_NEED_MORE) {
                return st;
            }
        }
        return CHIPS_DEC_NEED_MORE;
    }

    std::vector<uint8_t> FramePayload() const {
        return std::vector<uint8_t>(m_frame.payload, m_frame.payload + m_frame.payload_len);
    }

    chips_decoder_t m_dec{};
    uint8_t m_payload_buf[CHIPS_MAX_PAYLOAD]{};
    chips_frame_t m_frame{};
};

TEST(Crc, KermitCheckValue) {
    const uint8_t data[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    EXPECT_EQ(chips_crc16_kermit_compute(data, sizeof(data), CHIPS_CRC16_KERMIT_INITIAL), 0x2189u);
}

TEST(Crc, IncrementalMatchesOneShot) {
    const uint8_t data[] = {0x10, 0x20, 0x30, 0x40, 0x50, 0x60};
    const uint16_t one_shot = chips_crc16_kermit_compute(data, sizeof(data), CHIPS_CRC16_KERMIT_INITIAL);
    uint16_t inc = chips_crc16_kermit_compute(data, 2, CHIPS_CRC16_KERMIT_INITIAL);
    inc = chips_crc16_kermit_compute(data + 2, sizeof(data) - 2, inc);
    EXPECT_EQ(one_shot, inc);
}

TEST_F(CoreTest, EncodeRejectsBadArguments) {
    uint8_t out[CHIPS_WIRE_MAX];
    const uint8_t p[4] = {1, 2, 3, 4};

    EXPECT_EQ(chips_encode(0, CHIPS_DIR_REQUEST, k_cmd, p, 4, nullptr), 0u);
    EXPECT_EQ(chips_encode(0, CHIPS_DIR_REQUEST, k_cmd, nullptr, 4, out), 0u);
    EXPECT_EQ(chips_encode(0, CHIPS_DIR_REQUEST, k_cmd, p, CHIPS_MAX_PAYLOAD + 1u, out), 0u);
    EXPECT_EQ(chips_encode(0, CHIPS_DIR_REQUEST, uint8_t(CHIPS_CMD_MASK + 1u), p, 4, out), 0u);
}

TEST_F(CoreTest, EncodedFrameIsDelimitedAndStuffed) {
    const std::vector<uint8_t> payload{CHIPS_SYNC, 0x00, CHIPS_ESC, CHIPS_SYNC, CHIPS_ESC, 0x7F, CHIPS_SYNC};
    const auto wire = Encode(CHIPS_SYNC, CHIPS_DIR_REQUEST, k_cmd, payload);

    ASSERT_GE(wire.size(), size_t(CHIPS_FRAME_MIN + 2));
    ASSERT_LE(wire.size(), CHIPS_WIRE_SIZE(payload.size()));
    EXPECT_EQ(wire.front(), CHIPS_SYNC);
    EXPECT_EQ(wire.back(), CHIPS_SYNC);

    for (size_t i = 1; i + 1 < wire.size(); ++i) {
        EXPECT_NE(wire[i], CHIPS_SYNC) << i;
    }

    for (size_t i = 1; i + 1 < wire.size(); ++i) {
        if (wire[i] == CHIPS_ESC) {
            ASSERT_LT(i + 1, wire.size() - 1);
            EXPECT_TRUE(wire[i + 1] == CHIPS_SYNC_SUB || wire[i + 1] == CHIPS_ESC_SUB) << i;
            ++i;
        }
    }
}

TEST_F(CoreTest, RoundTripEmptyPayload) {
    const auto wire = Encode(0x42, CHIPS_DIR_REQUEST, k_cmd, {});
    ASSERT_FALSE(wire.empty());

    size_t consumed = 0;
    ASSERT_EQ(DecodeOnce(wire, &consumed), CHIPS_DEC_FRAME);
    EXPECT_EQ(consumed, wire.size());
    EXPECT_EQ(m_frame.seq, 0x42);
    EXPECT_EQ(m_frame.direction, CHIPS_DIR_REQUEST);
    EXPECT_EQ(m_frame.cmd, k_cmd);
    EXPECT_EQ(m_frame.payload_len, 0u);
}

TEST_F(CoreTest, RoundTripResponseWithSpecialBytes) {
    const std::vector<uint8_t> payload{0x01,          CHIPS_SYNC, CHIPS_ESC, CHIPS_SYNC_SUB,
                                       CHIPS_ESC_SUB, 0xFF,       CHIPS_ESC, CHIPS_ESC};
    const auto wire = Encode(CHIPS_ESC, CHIPS_DIR_RESPONSE, k_cmd, payload);
    ASSERT_FALSE(wire.empty());

    size_t consumed = 0;
    ASSERT_EQ(DecodeOnce(wire, &consumed), CHIPS_DEC_FRAME);
    ASSERT_EQ(consumed, wire.size());
    EXPECT_EQ(m_frame.seq, CHIPS_ESC);
    EXPECT_EQ(m_frame.direction, CHIPS_DIR_RESPONSE);
    EXPECT_EQ(m_frame.cmd, k_cmd);
    EXPECT_EQ(FramePayload(), payload);
}

TEST_F(CoreTest, RoundTripMaxPayloadWorstCaseStuffing) {
    const std::vector<uint8_t> payload(CHIPS_MAX_PAYLOAD, CHIPS_SYNC);
    const auto wire = Encode(CHIPS_SYNC, CHIPS_DIR_REQUEST, CHIPS_SYNC, payload);
    ASSERT_GE(wire.size(), size_t(CHIPS_WIRE_MAX - 2));

    size_t consumed = 0;
    ASSERT_EQ(DecodeOnce(wire, &consumed), CHIPS_DEC_FRAME);
    ASSERT_EQ(consumed, wire.size());
    EXPECT_EQ(m_frame.payload_len, size_t(CHIPS_MAX_PAYLOAD));
    EXPECT_EQ(FramePayload(), payload);
}

TEST_F(CoreTest, ByteByByteStreamingDecodes) {
    const std::vector<uint8_t> payload = {CHIPS_ESC, CHIPS_SYNC, 0x10, CHIPS_ESC};
    const auto wire = Encode(0x09, CHIPS_DIR_RESPONSE, k_cmd, payload);

    ASSERT_EQ(DecodeByteByByte(wire), CHIPS_DEC_FRAME);
    EXPECT_EQ(m_frame.seq, 0x09);
    EXPECT_EQ(FramePayload(), payload);
}

TEST_F(CoreTest, LeadingGarbageIsSkipped) {
    std::vector<uint8_t> stream = {0x11, 0x22, CHIPS_ESC, 0x33};
    const std::vector<uint8_t> payload = {0xAA, 0xBB};
    const auto wire = Encode(0x01, CHIPS_DIR_REQUEST, k_cmd, payload);
    stream.insert(stream.end(), wire.begin(), wire.end());

    size_t consumed = 0;
    ASSERT_EQ(DecodeOnce(stream, &consumed), CHIPS_DEC_FRAME);
    EXPECT_EQ(consumed, stream.size());
    EXPECT_EQ(FramePayload(), payload);
}

TEST_F(CoreTest, BackToBackFramesAndIdleSyncs) {
    const std::vector<uint8_t> p1 = {0x01, 0x02};
    const std::vector<uint8_t> p2 = {0x03, CHIPS_SYNC, 0x04};
    const auto w1 = Encode(1, CHIPS_DIR_REQUEST, k_cmd, p1);
    const auto w2 = Encode(2, CHIPS_DIR_RESPONSE, k_cmd, p2);

    std::vector<uint8_t> stream = w1;
    stream.push_back(CHIPS_SYNC);
    stream.push_back(CHIPS_SYNC);
    stream.insert(stream.end(), w2.begin(), w2.end());

    size_t consumed = 0;
    ASSERT_EQ(DecodeOnce(stream, &consumed), CHIPS_DEC_FRAME);
    EXPECT_EQ(m_frame.seq, 1);
    EXPECT_EQ(FramePayload(), p1);
    ASSERT_LT(consumed, stream.size());

    std::vector<uint8_t> remaining(stream.begin() + ptrdiff_t(consumed), stream.end());

    size_t consumed2 = 0;
    ASSERT_EQ(DecodeOnce(remaining, &consumed2), CHIPS_DEC_FRAME);
    EXPECT_EQ(m_frame.seq, 2);
    EXPECT_EQ(m_frame.direction, CHIPS_DIR_RESPONSE);
    EXPECT_EQ(FramePayload(), p2);
    EXPECT_EQ(consumed + consumed2, stream.size());
}

struct ExpectedFrame {
    uint8_t seq;
    chips_direction_t dir;
    uint8_t cmd;
    std::vector<uint8_t> payload;
};

class Rng {
  public:
    explicit Rng(uint32_t seed) : m_gen(seed) {}
    uint32_t Below(uint32_t n) { return static_cast<uint32_t>(m_gen() % n); }
    uint8_t Byte() { return static_cast<uint8_t>(m_gen() & 0xFFu); }
    bool Percent(uint32_t p) { return Below(100u) < p; }

  private:
    std::mt19937 m_gen;
};

uint8_t InterestingByte(Rng& rng) {
    switch (rng.Below(10u)) {
        case 0:
            return CHIPS_SYNC;
        case 1:
            return CHIPS_ESC;
        case 2:
            return CHIPS_SYNC_SUB;
        case 3:
            return CHIPS_ESC_SUB;
        default:
            return rng.Byte();
    }
}

ExpectedFrame RandomFrame(Rng& rng) {
    ExpectedFrame f;
    f.seq = InterestingByte(rng);
    f.dir = rng.Percent(50u) ? CHIPS_DIR_RESPONSE : CHIPS_DIR_REQUEST;
    f.cmd = uint8_t(rng.Byte() & CHIPS_CMD_MASK);
    const size_t len = rng.Percent(5u) ? size_t(CHIPS_MAX_PAYLOAD) : size_t(rng.Below(33u));
    f.payload.resize(len);
    for (auto& b : f.payload) {
        b = InterestingByte(rng);
    }
    return f;
}

bool NoiseDecodesAsFrame(const std::vector<uint8_t>& noise) {
    std::vector<uint8_t> probe;
    probe.reserve(noise.size() + 2u);
    probe.push_back(CHIPS_SYNC);
    probe.insert(probe.end(), noise.begin(), noise.end());
    probe.push_back(CHIPS_SYNC);

    chips_decoder_t dec{};
    uint8_t buf[CHIPS_MAX_PAYLOAD];
    chips_decoder_init(&dec, buf);
    chips_frame_t frame{};

    size_t pos = 0;
    while (pos < probe.size()) {
        size_t consumed = 0;
        const chips_dec_status_t st = chips_decode(&dec, probe.data() + pos, probe.size() - pos, &consumed, &frame);
        if (st == CHIPS_DEC_FRAME) {
            return true;
        }
        if (consumed == 0u) {
            break;
        }
        pos += consumed;
    }
    return false;
}

std::vector<uint8_t> RandomNoise(Rng& rng) {
    for (int attempt = 0; attempt < 100; ++attempt) {
        std::vector<uint8_t> noise(rng.Below(48u));
        for (auto& b : noise) {
            b = InterestingByte(rng);
        }
        if (!noise.empty() && rng.Percent(10u)) {
            noise.back() = CHIPS_ESC;
        }
        if (!NoiseDecodesAsFrame(noise)) {
            return noise;
        }
    }
    return {};
}

TEST_F(CoreTest, RoundTripFramesInterleavedWithNoise) {
    constexpr uint32_t kSeeds[] = {1u, 2u, 3u, 0xC0FFEEu, 0xDEADBEEFu};
    constexpr int kFramesPerSeed = 3000;

    for (const uint32_t seed : kSeeds) {
        SCOPED_TRACE(::testing::Message() << "seed=" << seed);
        chips_decoder_reset(&m_dec);
        Rng rng(seed);

        std::vector<ExpectedFrame> expected;
        std::vector<uint8_t> stream = RandomNoise(rng);
        for (int i = 0; i < kFramesPerSeed; ++i) {
            ExpectedFrame f = RandomFrame(rng);
            const auto wire = Encode(f.seq, f.dir, f.cmd, f.payload);
            ASSERT_FALSE(wire.empty()) << "encode failed for frame " << i;
            stream.insert(stream.end(), wire.begin(), wire.end());

            const auto noise = RandomNoise(rng);
            stream.insert(stream.end(), noise.begin(), noise.end());
            expected.push_back(std::move(f));
        }

        std::vector<ExpectedFrame> got;
        size_t errors = 0;
        size_t pos = 0;
        while (pos < stream.size()) {
            const size_t chunk = std::min<size_t>(1u + rng.Below(64u), stream.size() - pos);
            size_t off = 0;
            while (off < chunk) {
                size_t consumed = 0;
                const chips_dec_status_t st =
                    chips_decode(&m_dec, stream.data() + pos + off, chunk - off, &consumed, &m_frame);
                ASSERT_GT(consumed, 0u) << "decoder made no progress at offset " << pos + off;
                ASSERT_LE(consumed, chunk - off);
                off += consumed;

                if (st == CHIPS_DEC_FRAME) {
                    got.push_back({m_frame.seq, m_frame.direction, m_frame.cmd, FramePayload()});
                } else if (st != CHIPS_DEC_NEED_MORE) {
                    ASSERT_NE(st, CHIPS_DEC_ERR_ARG);
                    ++errors;
                }
            }
            pos += chunk;
        }

        ASSERT_EQ(got.size(), expected.size());
        for (size_t i = 0; i < expected.size(); ++i) {
            SCOPED_TRACE(::testing::Message() << "frame " << i);
            EXPECT_EQ(got[i].seq, expected[i].seq);
            EXPECT_EQ(got[i].dir, expected[i].dir);
            EXPECT_EQ(got[i].cmd, expected[i].cmd);
            EXPECT_EQ(got[i].payload, expected[i].payload);
        }
        EXPECT_GT(errors, 0u);
    }
}

TEST_F(CoreTest, DecodeRejectsBadArguments) {
    const uint8_t b = CHIPS_SYNC;
    size_t consumed = 123;

    EXPECT_EQ(chips_decode(nullptr, &b, 1, &consumed, &m_frame), CHIPS_DEC_ERR_ARG);
    EXPECT_EQ(consumed, 0u);
    EXPECT_EQ(chips_decode(&m_dec, nullptr, 1, &consumed, &m_frame), CHIPS_DEC_ERR_ARG);
    EXPECT_EQ(chips_decode(&m_dec, &b, 1, nullptr, &m_frame), CHIPS_DEC_ERR_ARG);
    EXPECT_EQ(chips_decode(&m_dec, &b, 1, &consumed, nullptr), CHIPS_DEC_ERR_ARG);
    EXPECT_EQ(chips_decode(&m_dec, nullptr, 0, &consumed, &m_frame), CHIPS_DEC_NEED_MORE);
}

TEST_F(CoreTest, CorruptedByteGivesCrcError) {
    const std::vector<uint8_t> payload = {0x10, 0x20, 0x30, 0x40};
    auto wire = Encode(0x05, CHIPS_DIR_REQUEST, k_cmd, payload);

    ASSERT_EQ(wire[3], 0x10);
    wire[3] = 0x11;

    size_t consumed = 0;
    EXPECT_EQ(DecodeOnce(wire, &consumed), CHIPS_DEC_ERR_CRC);
}

TEST_F(CoreTest, TooShortFrame) {
    const std::vector<uint8_t> wire = {CHIPS_SYNC, 0x01, 0x02, CHIPS_SYNC};
    size_t consumed = 0;
    EXPECT_EQ(DecodeOnce(wire, &consumed), CHIPS_DEC_ERR_TOO_SHORT);
}

TEST_F(CoreTest, InvalidEscapeSequence) {
    uint8_t bad = 0x00;
    while (bad == CHIPS_SYNC || bad == CHIPS_ESC || bad == CHIPS_SYNC_SUB || bad == CHIPS_ESC_SUB) {
        ++bad;
    }
    const std::vector<uint8_t> wire = {CHIPS_SYNC, 0x01, CHIPS_ESC, bad, 0x02, 0x03, CHIPS_SYNC};
    size_t consumed = 0;
    EXPECT_EQ(DecodeOnce(wire, &consumed), CHIPS_DEC_ERR_ESCAPE);
}

TEST_F(CoreTest, EscapeRightBeforeClosingSync) {
    const std::vector<uint8_t> wire = {CHIPS_SYNC, 0x01, 0x02, 0x03, 0x04, CHIPS_ESC, CHIPS_SYNC};
    size_t consumed = 0;
    EXPECT_EQ(DecodeOnce(wire, &consumed), CHIPS_DEC_ERR_ESCAPE);
}

TEST_F(CoreTest, OverlongFrameGivesTooLong) {
    uint8_t filler = 0x00;
    while (filler == CHIPS_SYNC || filler == CHIPS_ESC) {
        ++filler;
    }
    std::vector<uint8_t> wire;
    wire.push_back(CHIPS_SYNC);
    wire.insert(wire.end(), static_cast<size_t>(CHIPS_FRAME_MAX) + 1u, filler);
    wire.push_back(CHIPS_SYNC);

    size_t consumed = 0;
    EXPECT_EQ(DecodeOnce(wire, &consumed), CHIPS_DEC_ERR_TOO_LONG);
}

std::vector<uint8_t> FullFrameThenEscapePair() {
    uint8_t filler = 0x00;
    while (filler == CHIPS_SYNC || filler == CHIPS_ESC) {
        ++filler;
    }
    std::vector<uint8_t> wire;
    wire.push_back(CHIPS_SYNC);
    wire.insert(wire.end(), size_t(CHIPS_FRAME_MAX), filler);
    wire.push_back(CHIPS_ESC);
    wire.push_back(CHIPS_SYNC_SUB);
    wire.push_back(CHIPS_SYNC);
    return wire;
}

TEST_F(CoreTest, OverlongViaEscapedByteGivesTooLong) {
    const auto wire = FullFrameThenEscapePair();
    size_t consumed = 0;
    EXPECT_EQ(DecodeOnce(wire, &consumed), CHIPS_DEC_ERR_TOO_LONG);
}

TEST_F(CoreTest, OverlongViaEscapedByteSplitAcrossCalls) {
    const auto wire = FullFrameThenEscapePair();
    EXPECT_EQ(DecodeByteByByte(wire), CHIPS_DEC_ERR_TOO_LONG);
}

TEST_F(CoreTest, DecoderRecoversAfterError) {
    const std::vector<uint8_t> payload = {0xDE, 0xAD};
    const auto good = Encode(0x33, CHIPS_DIR_REQUEST, k_cmd, payload);

    const std::vector<uint8_t> bad = {CHIPS_SYNC, 0x01, 0x02, CHIPS_SYNC};
    size_t consumed = 0;
    ASSERT_EQ(DecodeOnce(bad, &consumed), CHIPS_DEC_ERR_TOO_SHORT);

    ASSERT_EQ(DecodeOnce(good, &consumed), CHIPS_DEC_FRAME);
    EXPECT_EQ(m_frame.seq, 0x33);
    EXPECT_EQ(FramePayload(), payload);
}

TEST_F(CoreTest, ResetReturnsDiscardedLength) {
    const std::vector<uint8_t> partial = {CHIPS_SYNC, 0x01, 0x02, 0x03};
    size_t consumed = 0;
    ASSERT_EQ(DecodeOnce(partial, &consumed), CHIPS_DEC_NEED_MORE);
    EXPECT_EQ(chips_decoder_reset(&m_dec), 3u);
    EXPECT_EQ(chips_decoder_reset(&m_dec), 0u);
    EXPECT_EQ(chips_decoder_reset(nullptr), 0u);
}

TEST_F(CoreTest, InitWithNullDecoderIsNoOp) {
    chips_decoder_init(nullptr, m_payload_buf);
    SUCCEED();
}

}  // namespace
