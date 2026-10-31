/*
 * Copyright(c) 2026 Nikolai Fedotenko.
 * Distributed under the MIT License (http://opensource.org/licenses/MIT)
 */

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include <steroids/queues/mpsc_queue.h>

using steroids::mpsc_queue;

TEST(MpscQueue, RoundtripAndOrderSingleThread) {
    mpsc_queue<int, 8> q;
    for (int i = 0; i < 8; ++i) {
        ASSERT_TRUE(q.enqueue(i));
    }
    EXPECT_FALSE(q.enqueue(8)); // full
    int v = -1;
    for (int i = 0; i < 8; ++i) {
        ASSERT_TRUE(q.dequeue(v));
        EXPECT_EQ(v, i);
    }
    EXPECT_FALSE(q.dequeue(v));
    // reusable after wrap
    ASSERT_TRUE(q.enqueue(99));
    ASSERT_TRUE(q.dequeue(v));
    EXPECT_EQ(v, 99);
}

TEST(MpscQueue, MoveOnlyElements) {
    mpsc_queue<std::unique_ptr<int>, 4> q;
    ASSERT_TRUE(q.enqueue(std::make_unique<int>(7)));
    std::unique_ptr<int> p;
    ASSERT_TRUE(q.dequeue(p));
    ASSERT_TRUE(p);
    EXPECT_EQ(*p, 7);
}

namespace {
struct Counted {
    static inline std::atomic<int> alive{0};
    int v{0};
    Counted() { ++alive; }
    explicit Counted(int x) : v(x) { ++alive; }
    Counted(const Counted& o) : v(o.v) { ++alive; }
    Counted(Counted&& o) noexcept : v(o.v) { ++alive; }
    Counted& operator=(const Counted&) = default;
    Counted& operator=(Counted&&) noexcept = default;
    ~Counted() { --alive; }
};
} // namespace

TEST(MpscQueue, DestructorDrainsLeftovers) {
    {
        mpsc_queue<Counted, 8> q;
        for (int i = 0; i < 5; ++i) {
            ASSERT_TRUE(q.emplace(i));
        }
        EXPECT_EQ(Counted::alive.load(), 5);
    }
    EXPECT_EQ(Counted::alive.load(), 0);
}

// Four producers, one consumer: every element arrives exactly once and each
// producer's stream stays FIFO. Run under TSan/ASan in CI.
TEST(MpscQueue, FourProducersStress) {
    constexpr std::uint64_t per_producer = 100'000;
    constexpr int producers = 4;
    mpsc_queue<std::uint64_t, 1024> q;

    std::vector<std::thread> threads;
    threads.reserve(producers);
    for (int p = 0; p < producers; ++p) {
        threads.emplace_back([&q, p] {
            for (std::uint64_t i = 0; i < per_producer;) {
                // tag: high bits producer id, low bits sequence
                if (q.enqueue((static_cast<std::uint64_t>(p) << 32) | i)) {
                    ++i;
                }
            }
        });
    }

    std::uint64_t next_seq[producers] = {};
    std::uint64_t received = 0;
    std::uint64_t v = 0;
    while (received < per_producer * producers) {
        if (q.dequeue(v)) {
            const auto p = static_cast<int>(v >> 32);
            const std::uint64_t seq = v & 0xFFFFFFFFu;
            ASSERT_LT(p, producers);
            ASSERT_EQ(seq, next_seq[p]) << "per-producer FIFO broken";
            ++next_seq[p];
            ++received;
        }
    }
    for (auto& t : threads) {
        t.join();
    }
    EXPECT_FALSE(q.dequeue(v));
}
