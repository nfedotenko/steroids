/*
 * Copyright(c) 2026 Nikolai Fedotenko.
 * Distributed under the MIT License (http://opensource.org/licenses/MIT)
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <string_view>
#include <thread>
#include <vector>

#include <steroids/containers/id_map.h>

using steroids::fnv1a_32;
using steroids::id_map;

TEST(IdMap, Fnv1aIsConstexprAndStable) {
    static_assert(fnv1a_32("") == 2166136261u);
    constexpr auto h1 = fnv1a_32("order id={}");
    constexpr auto h2 = fnv1a_32("order id={}!");
    static_assert(h1 != h2);
    EXPECT_EQ(fnv1a_32(std::string_view{"abc"}), fnv1a_32("abc"));
}

TEST(IdMap, PutGetRoundtrip) {
    id_map<const char*, 64> m;
    EXPECT_FALSE(m.contains(42));
    EXPECT_EQ(m.get(42, "fallback"), std::string_view{"fallback"});

    ASSERT_TRUE(m.put(42, "hello"));
    EXPECT_TRUE(m.contains(42));
    EXPECT_EQ(m.get(42), std::string_view{"hello"});

    ASSERT_TRUE(m.put(42, "rebound")); // same id: last write wins
    EXPECT_EQ(m.get(42), std::string_view{"rebound"});
}

TEST(IdMap, ZeroIdIsRejected) {
    id_map<int, 16> m;
    EXPECT_FALSE(m.put(0, 1));
    EXPECT_FALSE(m.contains(0));
    EXPECT_EQ(m.get(0, -1), -1);
}

TEST(IdMap, CollidingIdsProbe) {
    id_map<int, 16> m;
    // ids congruent modulo capacity all land on the same home slot
    ASSERT_TRUE(m.put(1, 100));
    ASSERT_TRUE(m.put(17, 200));
    ASSERT_TRUE(m.put(33, 300));
    EXPECT_EQ(m.get(1), 100);
    EXPECT_EQ(m.get(17), 200);
    EXPECT_EQ(m.get(33), 300);
    EXPECT_FALSE(m.contains(49));
}

TEST(IdMap, FullTableRefusesNewIds) {
    id_map<int, 4> m;
    ASSERT_TRUE(m.put(1, 1));
    ASSERT_TRUE(m.put(2, 2));
    ASSERT_TRUE(m.put(3, 3));
    ASSERT_TRUE(m.put(4, 4));
    EXPECT_FALSE(m.put(5, 5)); // full
    EXPECT_TRUE(m.put(2, 22)); // existing id still updatable
    EXPECT_EQ(m.get(2), 22);
}

// Writers register distinct ids while readers hammer lookups of already
// registered ones. Run under TSan in CI.
TEST(IdMap, ConcurrentWritersAndReaders) {
    static id_map<std::uint64_t, 4096> m;
    ASSERT_TRUE(m.put(fnv1a_32("hot"), 777));

    std::vector<std::thread> threads;
    for (int w = 0; w < 2; ++w) {
        threads.emplace_back([w] {
            for (std::uint32_t i = 1; i <= 500; ++i) {
                const std::uint32_t id =
                    (static_cast<std::uint32_t>(w + 1) << 16) | i;
                ASSERT_TRUE(m.put(id, id * 10ull));
            }
        });
    }
    for (int r = 0; r < 2; ++r) {
        threads.emplace_back([] {
            for (int i = 0; i < 200'000; ++i) {
                ASSERT_EQ(m.get(fnv1a_32("hot")), 777ull);
            }
        });
    }
    for (auto& t : threads) {
        t.join();
    }
    for (int w = 0; w < 2; ++w) {
        for (std::uint32_t i = 1; i <= 500; ++i) {
            const std::uint32_t id =
                (static_cast<std::uint32_t>(w + 1) << 16) | i;
            ASSERT_EQ(m.get(id), id * 10ull);
        }
    }
}
