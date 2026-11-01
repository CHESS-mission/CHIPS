/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 EPFL Spacecraft Team
 */

#ifndef CHIPS_TEST_UTIL_H
#define CHIPS_TEST_UTIL_H

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <iomanip>
#include <limits>
#include <ostream>
#include <vector>

#include "chips/core.h"
#include "chips/port.h"

constexpr uint64_t ToNs(chips_time_t t) {
    return static_cast<uint64_t>(t.tv_sec) * CHIPS_NSEC_PER_SEC + t.tv_nsec;
}

constexpr chips_time_t FromNs(uint64_t ns) {
    return chips_time_t{static_cast<uint32_t>(ns / CHIPS_NSEC_PER_SEC), static_cast<uint32_t>(ns % CHIPS_NSEC_PER_SEC)};
}

constexpr chips_time_t operator+(chips_time_t a, chips_time_t b) {
    return FromNs(ToNs(a) + ToNs(b));
}

constexpr chips_time_t operator-(chips_time_t a, chips_time_t b) {
    return (a.tv_nsec >= b.tv_nsec) ? chips_time_t{static_cast<uint32_t>(a.tv_sec - b.tv_sec), a.tv_nsec - b.tv_nsec}
                                    : chips_time_t{static_cast<uint32_t>(a.tv_sec - b.tv_sec - 1u),
                                                   a.tv_nsec + CHIPS_NSEC_PER_SEC - b.tv_nsec};
}

constexpr chips_time_t operator*(uint64_t k, chips_time_t t) {
    return FromNs(k * ToNs(t));
}

inline chips_time_t& operator+=(chips_time_t& a, chips_time_t b) {
    return a = a + b;
}

constexpr bool operator==(chips_time_t a, chips_time_t b) {
    return a.tv_sec == b.tv_sec && a.tv_nsec == b.tv_nsec;
}

constexpr bool operator!=(chips_time_t a, chips_time_t b) {
    return !(a == b);
}

inline std::ostream& operator<<(std::ostream& os, chips_time_t t) {
    return os << t.tv_sec << '.' << std::setw(9) << std::setfill('0') << t.tv_nsec << std::setfill(' ') << 's';
}

namespace chips_test {

constexpr chips_time_t Ns(uint64_t ns) {
    return FromNs(ns);
}

constexpr chips_time_t Ms(uint64_t ms) {
    return FromNs(ms * 1000000u);
}

using Bytes = std::vector<uint8_t>;

constexpr chips_time_t K_RESEND_PERIOD = Ms(1000u);
constexpr uint8_t K_MAX_RESENDS = 2u;

constexpr chips_time_t K_TIMEOUT = (K_MAX_RESENDS + 1u) * K_RESEND_PERIOD;
constexpr chips_time_t K_CACHE_WINDOW = (K_MAX_RESENDS + 2u) * K_RESEND_PERIOD;

inline chips_timing_t TestTiming() {
    chips_timing_t t{};
    t.resend_period = K_RESEND_PERIOD;
    t.max_resends = K_MAX_RESENDS;
    return t;
}

inline bool Has(uint32_t ev, uint32_t flag) {
    return (ev & flag) != 0;
}

inline Bytes Encode(uint8_t seq, chips_direction_t dir, uint8_t cmd, const Bytes& payload = {}) {
    uint8_t buf[CHIPS_WIRE_MAX];
    const size_t n = chips_encode(seq, dir, cmd, payload.empty() ? nullptr : payload.data(), payload.size(), buf);
    EXPECT_GT(n, 0u);
    EXPECT_LE(n, sizeof(buf));
    return Bytes(buf, buf + n);
}

inline Bytes Req(uint8_t seq, uint8_t cmd, const Bytes& payload = {}) {
    return Encode(seq, CHIPS_DIR_REQUEST, cmd, payload);
}

inline Bytes Resp(uint8_t seq, uint8_t cmd, const Bytes& payload = {}) {
    return Encode(seq, CHIPS_DIR_RESPONSE, cmd, payload);
}

inline Bytes Repeat(const Bytes& b, int times) {
    Bytes out;
    for (int i = 0; i < times; ++i) {
        out.insert(out.end(), b.begin(), b.end());
    }
    return out;
}

inline Bytes Concat(const Bytes& a, const Bytes& b) {
    Bytes out = a;
    out.insert(out.end(), b.begin(), b.end());
    return out;
}

inline Bytes Pattern(size_t n, uint8_t mul = 37u, uint8_t add = 0u) {
    Bytes b(n);
    for (size_t i = 0; i < n; ++i) {
        b[i] = static_cast<uint8_t>(i * mul + add);
    }
    return b;
}

inline bool ReplaceFirst(Bytes& b, uint8_t from, uint8_t to) {
    const auto it = std::find(b.begin(), b.end(), from);
    if (it == b.end()) {
        return false;
    }
    *it = to;
    return true;
}

struct FakePort {
    chips_time_t now{};

    std::deque<uint8_t> rx;
    Bytes tx;

    size_t max_write = std::numeric_limits<size_t>::max();
    size_t max_read = std::numeric_limits<size_t>::max();

    bool fail_write = false;
    bool fail_read = false;

    int write_calls = 0;
    int read_calls = 0;

    void Feed(const Bytes& b) { rx.insert(rx.end(), b.begin(), b.end()); }

    chips_port_t Port() {
        chips_port_t p{};
        p.ctx = this;
        p.write = &FakePort::Write;
        p.read = &FakePort::Read;
        p.now = &FakePort::Now;
        return p;
    }

    static chips_io_status_t Write(void* ctx, const uint8_t* buf, size_t len, size_t* written) {
        auto* self = static_cast<FakePort*>(ctx);
        self->write_calls++;
        *written = 0;
        if (self->fail_write) {
            return CHIPS_IO_ERROR;
        }
        const size_t n = std::min(len, self->max_write);
        self->tx.insert(self->tx.end(), buf, buf + n);
        *written = n;
        return CHIPS_IO_OK;
    }

    static chips_io_status_t Read(void* ctx, uint8_t* buf, size_t cap, size_t* n) {
        auto* self = static_cast<FakePort*>(ctx);
        self->read_calls++;
        *n = 0;
        if (self->fail_read) {
            return CHIPS_IO_ERROR;
        }
        const size_t k = std::min(cap, std::min(self->max_read, self->rx.size()));
        for (size_t i = 0; i < k; ++i) {
            buf[i] = self->rx.front();
            self->rx.pop_front();
        }
        *n = k;
        return CHIPS_IO_OK;
    }

    static chips_time_t Now(void* ctx) { return static_cast<FakePort*>(ctx)->now; }
};

struct EventTally {
    unsigned long steps = 0;
    std::array<unsigned long, 32> per_bit{};

    void Add(uint32_t ev) {
        steps++;
        for (unsigned i = 0; i < 32; ++i) {
            if (ev & (1u << i)) {
                per_bit[i]++;
            }
        }
    }

    unsigned long Count(uint32_t flag) const {
        EXPECT_TRUE(flag != 0 && (flag & (flag - 1)) == 0) << "Count() takes a single event bit";
        for (unsigned i = 0; i < 32; ++i) {
            if (flag == (1u << i)) {
                return per_bit[i];
            }
        }
        return 0;
    }
};

template <typename StepFn>
uint32_t StepUntil(StepFn&& step, uint32_t flag, int max_steps = 1000) {
    uint32_t all = 0;
    for (int i = 0; i < max_steps; ++i) {
        all |= step();
        if (Has(all, flag)) {
            break;
        }
    }
    return all;
}

template <typename StepFn>
uint32_t Drain(StepFn&& step, const FakePort& fake, int max_steps = 100) {
    uint32_t all = step();
    for (int i = 0; i < max_steps && !fake.rx.empty(); ++i) {
        all |= step();
    }
    return all;
}

struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed) {}
    uint64_t Next() {
        uint64_t z = (s += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    uint32_t Below(uint32_t n) { return n == 0 ? 0 : static_cast<uint32_t>(Next() % n); }
    bool Chance(double p) { return p > 0 && static_cast<double>(Next() >> 11) * 0x1.0p-53 < p; }
};

}  // namespace chips_test

#endif /* CHIPS_TEST_UTIL_H */
