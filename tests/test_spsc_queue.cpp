/*
 * Copyright(c) 2025 Nikolai Fedotenko.
 * Distributed under the MIT License (http://opensource.org/licenses/MIT)
 */

#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <string>
#include <thread>

#include <steroids/queues/spsc_queue.h>

using steroids::spsc_queue;

TEST(SpscQueue, RoundtripAndOrder) {
    spsc_queue<int, 8> q;
    EXPECT_EQ(q.capacity(), 7u);
    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(q.enqueue(i));
    }
    int v = -1;
    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(q.dequeue(v));
        EXPECT_EQ(v, i);
    }
    EXPECT_FALSE(q.dequeue(v));
}

TEST(SpscQueue, FullThenEmptyThenReusable) {
    spsc_queue<int, 4> q; // capacity 3
    EXPECT_TRUE(q.enqueue(1));
    EXPECT_TRUE(q.enqueue(2));
    EXPECT_TRUE(q.enqueue(3));
    EXPECT_FALSE(q.enqueue(4)); // full

    int v = 0;
    EXPECT_TRUE(q.dequeue(v));
    EXPECT_TRUE(q.enqueue(4)); // slot freed
    EXPECT_TRUE(q.dequeue(v));
    EXPECT_TRUE(q.dequeue(v));
    EXPECT_TRUE(q.dequeue(v));
    EXPECT_EQ(v, 4);
    EXPECT_FALSE(q.dequeue(v));
}

TEST(SpscQueue, MoveOnlyElements) {
    spsc_queue<std::unique_ptr<int>, 8> q;
    ASSERT_TRUE(q.enqueue(std::make_unique<int>(42)));
    std::unique_ptr<int> p;
    ASSERT_TRUE(q.dequeue(p));
    ASSERT_TRUE(p);
    EXPECT_EQ(*p, 42);
}

namespace {
struct Counted {
    static inline std::atomic<int> alive{0};
    Counted() { ++alive; }
    Counted(const Counted&) { ++alive; }
    Counted(Counted&&) noexcept { ++alive; }
    Counted& operator=(const Counted&) = default;
    Counted& operator=(Counted&&) noexcept = default;
    ~Counted() { --alive; }
};
} // namespace

TEST(SpscQueue, ClearDestroysRemaining) {
    {
        spsc_queue<Counted, 8> q;
        for (int i = 0; i < 6; ++i) {
            ASSERT_TRUE(q.emplace());
        }
        EXPECT_EQ(Counted::alive.load(), 6);
        q.clear();
        EXPECT_EQ(Counted::alive.load(), 0);
        ASSERT_TRUE(q.emplace()); // usable after clear
    }
    // destructor drains what is left
    EXPECT_EQ(Counted::alive.load(), 0);
}

TEST(SpscQueue, ApproxSizeSingleThread) {
    spsc_queue<int, 16> q;
    EXPECT_EQ(q.approx_size(), 0u);
    for (int i = 0; i < 10; ++i) {
        ASSERT_TRUE(q.enqueue(i));
    }
    EXPECT_EQ(q.approx_size(), 10u);
    int v = 0;
    ASSERT_TRUE(q.dequeue(v));
    EXPECT_EQ(q.approx_size(), 9u);
}

// Producer and consumer on their own threads: every element arrives intact
// and in order. Run under TSan/ASan in CI.
TEST(SpscQueue, TwoThreadsStress) {
    constexpr std::size_t N = 1'000'000;
    spsc_queue<std::size_t, 1024> q;

    std::thread producer([&] {
        std::size_t i = 0;
        while (i < N) {
            if (q.enqueue(i)) {
                ++i;
            }
        }
    });

    std::size_t expect = 0;
    std::size_t v = 0;
    while (expect < N) {
        if (q.dequeue(v)) {
            ASSERT_EQ(v, expect);
            ++expect;
        }
    }
    producer.join();
    EXPECT_EQ(q.approx_size(), 0u);
}

// Strings exercise non-trivial construct/move/destroy across the ring seam.
TEST(SpscQueue, TwoThreadsStrings) {
    constexpr int N = 100'000;
    spsc_queue<std::string, 256> q;

    std::thread producer([&] {
        int i = 0;
        while (i < N) {
            if (q.emplace("payload-" + std::to_string(i))) {
                ++i;
            }
        }
    });

    int expect = 0;
    std::string s;
    while (expect < N) {
        if (q.dequeue(s)) {
            ASSERT_EQ(s, "payload-" + std::to_string(expect));
            ++expect;
        }
    }
    producer.join();
}
