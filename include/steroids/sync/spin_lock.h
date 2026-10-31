/*
 * Copyright(c) 2026 Nikolai Fedotenko.
 * Distributed under the MIT License (http://opensource.org/licenses/MIT)
 */

#pragma once

#include <atomic>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) ||            \
    defined(_M_IX86)
#include <immintrin.h>
#endif

namespace steroids {

inline void cpu_pause() noexcept {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) ||            \
    defined(_M_IX86)
    _mm_pause();
#elif defined(__aarch64__) || defined(_M_ARM64)
    __asm__ volatile("yield");
#endif
}

// Test-and-test-and-set spin lock: the retry loop spins on a plain load, so
// waiters hammer their own cache line copy instead of the interconnect, and
// pause between probes. For short critical sections only; it never yields
// to the scheduler.
class spin_lock {
public:
    void lock() noexcept {
        while (flag_.test_and_set(std::memory_order_acquire)) {
            while (flag_.test(std::memory_order_relaxed)) {
                cpu_pause();
            }
        }
    }

    bool try_lock() noexcept {
        return !flag_.test_and_set(std::memory_order_acquire);
    }

    void unlock() noexcept { flag_.clear(std::memory_order_release); }

private:
    std::atomic_flag flag_ = ATOMIC_FLAG_INIT;
};

} // namespace steroids
