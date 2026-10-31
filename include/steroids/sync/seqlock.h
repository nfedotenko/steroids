/*
 * Copyright(c) 2026 Nikolai Fedotenko.
 * Distributed under the MIT License (http://opensource.org/licenses/MIT)
 */

#pragma once

#include <steroids/sync/spin_lock.h> // cpu_pause

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace steroids {

//------------------------------------------------------------------------------
// Seqlock: one writer publishes snapshots of a trivially copyable T, any
// number of readers take them wait-free on the writer's side — the writer
// never blocks on readers, readers retry if a write overlapped their copy.
// The classic tool for market-data style "latest value wins" sharing.
//
// The payload is stored as relaxed atomic words rather than a plain T, the
// construction from Boehm's "Can seqlocks get along with programming
// language memory models?": no data race exists even formally, so TSan is
// clean by design, at the cost of a word-wise copy.
//
// Contract: a single writer (or writers serialized externally). load() is
// lock-free but not wait-free — a reader retries while a write overlaps.
//------------------------------------------------------------------------------
template <typename T>
    requires(std::is_trivially_copyable_v<T>)
class seqlock {
    static constexpr std::size_t word_count =
        (sizeof(T) + sizeof(std::uint64_t) - 1) / sizeof(std::uint64_t);

public:
    seqlock() noexcept = default;

    explicit seqlock(const T& initial) noexcept { store(initial); }

    seqlock(const seqlock&) = delete;
    seqlock& operator=(const seqlock&) = delete;

    // Writer thread only.
    void store(const T& value) noexcept {
        std::uint64_t src[word_count] = {};
        std::memcpy(src, &value, sizeof(T));

        const std::uint32_t s = seq_.load(std::memory_order_relaxed);
        seq_.store(s + 1, std::memory_order_relaxed); // odd: write started
        std::atomic_thread_fence(std::memory_order_release);
        for (std::size_t i = 0; i < word_count; ++i) {
            words_[i].store(src[i], std::memory_order_relaxed);
        }
        seq_.store(s + 2, std::memory_order_release); // even: published
    }

    // Any thread.
    T load() const noexcept {
        std::uint64_t dst[word_count];
        for (;;) {
            const std::uint32_t s1 = seq_.load(std::memory_order_acquire);
            if (s1 & 1u) { // write in progress
                cpu_pause();
                continue;
            }
            for (std::size_t i = 0; i < word_count; ++i) {
                dst[i] = words_[i].load(std::memory_order_relaxed);
            }
            std::atomic_thread_fence(std::memory_order_acquire);
            if (seq_.load(std::memory_order_relaxed) == s1) {
                break;
            }
            cpu_pause(); // torn by a concurrent write; retry
        }
        T out;
        std::memcpy(&out, dst, sizeof(T));
        return out;
    }

private:
    static constexpr std::size_t cacheline = 64;

    alignas(cacheline) std::atomic<std::uint32_t> seq_{0};
    std::atomic<std::uint64_t> words_[word_count]{};
};

} // namespace steroids
