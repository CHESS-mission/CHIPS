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
#include <limits>
#include <utility>
#include <vector>

#include "chips/core.h"
#include "chips/port.h"

namespace chips_test {

using Bytes = std::vector<uint8_t>;

using IoStatus = decltype(std::declval<chips_port_t>().write(nullptr, nullptr, 0, nullptr));

inline const IoStatus k_io_fail = static_cast<IoStatus>(static_cast<int>(CHIPS_IO_OK) + 1);

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
    uint32_t now = 0;

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
        p.now_us = &FakePort::Now;
        return p;
    }

    static IoStatus Write(void* ctx, const uint8_t* buf, size_t len, size_t* written) {
        auto* self = static_cast<FakePort*>(ctx);
        self->write_calls++;
        *written = 0;
        if (self->fail_write) {
            return k_io_fail;
        }
        const size_t n = std::min(len, self->max_write);
        self->tx.insert(self->tx.end(), buf, buf + n);
        *written = n;
        return CHIPS_IO_OK;
    }

    static IoStatus Read(void* ctx, uint8_t* buf, size_t cap, size_t* n) {
        auto* self = static_cast<FakePort*>(ctx);
        self->read_calls++;
        *n = 0;
        if (self->fail_read) {
            return k_io_fail;
        }
        const size_t k = std::min(cap, std::min(self->max_read, self->rx.size()));
        for (size_t i = 0; i < k; ++i) {
            buf[i] = self->rx.front();
            self->rx.pop_front();
        }
        *n = k;
        return CHIPS_IO_OK;
    }

    static uint32_t Now(void* ctx) { return static_cast<FakePort*>(ctx)->now; }
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
