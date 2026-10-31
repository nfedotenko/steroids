/*
 * Copyright(c) 2026 Nikolai Fedotenko.
 * Distributed under the MIT License (http://opensource.org/licenses/MIT)
 */

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <thread>

#include <steroids/sync/seqlock.h>

using steroids::seqlock;

namespace {

// Internally consistent payload: readers can detect a torn copy.
struct Tick {
    std::uint64_t a;
    std::uint64_t twice_a;
    std::uint64_t a_plus_one;
    std::uint32_t tail; // odd size paths: struct is 28 bytes
};

Tick make_tick(std::uint64_t a) {
    return {a, a * 2, a + 1, static_cast<std::uint32_t>(a & 0xFFFFFFFFu)};
}

bool consistent(const Tick& t) {
    return t.twice_a == t.a * 2 && t.a_plus_one == t.a + 1 &&
           t.tail == (t.a & 0xFFFFFFFFu);
}

} // namespace

TEST(Seqlock, RoundtripSingleThread) {
    seqlock<Tick> s(make_tick(7));
    EXPECT_TRUE(consistent(s.load()));
    EXPECT_EQ(s.load().a, 7u);

    s.store(make_tick(42));
    EXPECT_EQ(s.load().a, 42u);
    EXPECT_TRUE(consistent(s.load()));
}

TEST(Seqlock, SmallAndOddSizedPayloads) {
    seqlock<std::uint32_t> small(0xDEADBEEFu);
    EXPECT_EQ(small.load(), 0xDEADBEEFu);

    struct Odd {
        char bytes[13];
    };
    Odd o{};
    o.bytes[0] = 'x';
    o.bytes[12] = 'z';
    seqlock<Odd> odd(o);
    const Odd back = odd.load();
    EXPECT_EQ(back.bytes[0], 'x');
    EXPECT_EQ(back.bytes[12], 'z');
}

// One writer hammers snapshots, readers must never observe a torn value.
// This is the test that catches a broken fence protocol; run under TSan in
// CI, where the atomic-word construction must also stay silent.
TEST(Seqlock, ReadersNeverSeeTornWrites) {
    seqlock<Tick> s(make_tick(0));
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> reads{0};

    auto reader = [&] {
        std::uint64_t last = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            const Tick t = s.load();
            ASSERT_TRUE(consistent(t)) << "torn read at a=" << t.a;
            ASSERT_GE(t.a, last) << "went back in time";
            last = t.a;
            reads.fetch_add(1, std::memory_order_relaxed);
        }
    };

    std::thread r1(reader);
    std::thread r2(reader);
    std::thread r3(reader);

    for (std::uint64_t a = 1; a <= 300'000; ++a) {
        s.store(make_tick(a));
    }
    stop.store(true, std::memory_order_relaxed);
    r1.join();
    r2.join();
    r3.join();

    EXPECT_EQ(s.load().a, 300'000u);
    EXPECT_GT(reads.load(), 0u);
}
