/*
 * Copyright(c) 2026 Nikolai Fedotenko.
 * Distributed under the MIT License (http://opensource.org/licenses/MIT)
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstring>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <steroids/memory/pool_allocator.h>

using steroids::pool_allocate;
using steroids::pool_allocator;
using steroids::pool_deallocate;

TEST(PoolAllocator, ClassRouting) {
    using steroids::detail::class_block_size;
    using steroids::detail::class_index;
    EXPECT_EQ(class_index(1), 0u);
    EXPECT_EQ(class_index(64), 0u);
    EXPECT_EQ(class_index(65), 1u);
    EXPECT_EQ(class_index(128), 1u);
    EXPECT_EQ(class_index(4096), 6u);
    EXPECT_EQ(class_index(16384), 8u);
    EXPECT_EQ(class_block_size(0), 64u);
    EXPECT_EQ(class_block_size(8), 16384u);
}

TEST(PoolAllocator, ReusesFreedBlock) {
    void* a = pool_allocate(100);
    ASSERT_NE(a, nullptr);
    pool_deallocate(a, 100);
    void* b = pool_allocate(100);
    EXPECT_EQ(a, b); // LIFO free list hands the hot block back
    pool_deallocate(b, 100);
}

TEST(PoolAllocator, DistinctLiveBlocks) {
    constexpr int N = 10'000;
    std::set<void*> seen;
    std::vector<void*> ptrs;
    ptrs.reserve(N);
    for (int i = 0; i < N; ++i) {
        void* p = pool_allocate(48);
        ASSERT_NE(p, nullptr);
        ASSERT_TRUE(seen.insert(p).second) << "block handed out twice";
        ptrs.push_back(p);
    }
    for (void* p : ptrs) {
        pool_deallocate(p, 48);
    }
}

TEST(PoolAllocator, AlignmentMatchesClass) {
    for (std::size_t size : {1u, 64u, 100u, 1000u, 4096u}) {
        void* p = pool_allocate(size);
        const auto addr = reinterpret_cast<std::uintptr_t>(p);
        const std::size_t block = std::max<std::size_t>(std::bit_ceil(size), 64);
        EXPECT_EQ(addr % block, 0u) << "size " << size;
        pool_deallocate(p, size);
    }
}

TEST(PoolAllocator, LargeAllocationsBypassThePool) {
    const std::size_t big = (1u << 14) + 1; // one past the largest class
    void* p = pool_allocate(big);
    ASSERT_NE(p, nullptr);
    std::memset(p, 0xAB, big); // must be really writable
    pool_deallocate(p, big);

    void* huge = pool_allocate(1u << 22);
    ASSERT_NE(huge, nullptr);
    std::memset(huge, 0xCD, 1u << 22);
    pool_deallocate(huge, 1u << 22);
}

TEST(PoolAllocator, WritesDoNotCorruptNeighbours) {
    void* a = pool_allocate(64);
    void* b = pool_allocate(64);
    std::memset(a, 0x11, 64);
    std::memset(b, 0x22, 64);
    EXPECT_EQ(static_cast<unsigned char*>(a)[63], 0x11);
    EXPECT_EQ(static_cast<unsigned char*>(b)[0], 0x22);
    pool_deallocate(a, 64);
    pool_deallocate(b, 64);
}

TEST(PoolAllocator, StlAdapterRunsAVector) {
    std::vector<int, pool_allocator<int>> v;
    for (int i = 0; i < 100'000; ++i) {
        v.push_back(i);
    }
    EXPECT_EQ(v[99'999], 99'999);

    std::basic_string<char, std::char_traits<char>, pool_allocator<char>> s;
    for (int i = 0; i < 1000; ++i) {
        s += "chunk";
    }
    EXPECT_EQ(s.size(), 5000u);
}

TEST(PoolAllocator, AdapterRejectsOverflow) {
    pool_allocator<std::uint64_t> a;
    EXPECT_THROW((void)a.allocate(~std::size_t{0} / 4), std::bad_alloc);
}

// Each thread owns its cache: parallel allocate/free on separate threads
// must not interfere. Run under TSan in CI.
TEST(PoolAllocator, PerThreadCachesAreIndependent) {
    auto hammer = [] {
        std::vector<void*> ptrs;
        ptrs.reserve(1000);
        for (int round = 0; round < 200; ++round) {
            for (int i = 0; i < 1000; ++i) {
                void* p = pool_allocate(static_cast<std::size_t>(
                    64 + (i % 7) * 100));
                static_cast<char*>(p)[0] = static_cast<char>(i);
                ptrs.push_back(p);
            }
            for (int i = 0; i < 1000; ++i) {
                pool_deallocate(ptrs[static_cast<std::size_t>(i)],
                                static_cast<std::size_t>(64 + (i % 7) * 100));
            }
            ptrs.clear();
        }
    };
    std::thread t1(hammer);
    std::thread t2(hammer);
    std::thread t3(hammer);
    t1.join();
    t2.join();
    t3.join();
}
