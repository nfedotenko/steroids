/*
 * Copyright(c) 2026 Nikolai Fedotenko.
 * Distributed under the MIT License (http://opensource.org/licenses/MIT)
 *
 * Shared benchmark plumbing: serialized tick reads, tick→ns calibration,
 * thread pinning and percentile summaries. Same methodology as the
 * steroidslog harness.
 */

#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

#if defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
#elif defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <intrin.h>
#elif defined(__APPLE__)
#include <time.h>
#endif

namespace steroids_bench {

inline uint64_t ts_begin() noexcept {
#if defined(__x86_64__) || defined(__i386__) ||                               \
    (defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86)))
    _mm_lfence();
    return __rdtsc();
#elif defined(__aarch64__) && !defined(__APPLE__)
    uint64_t v;
    __asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(v)::"memory");
    return v;
#elif defined(__APPLE__)
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
#else
    return static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}

inline uint64_t ts_end() noexcept {
#if defined(__x86_64__) || defined(__i386__) ||                               \
    (defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86)))
    unsigned aux = 0;
    const uint64_t t = __rdtscp(&aux);
    _mm_lfence();
    return t;
#else
    return ts_begin();
#endif
}

inline double measure_ns_per_tick() {
    const auto s0 = std::chrono::steady_clock::now();
    const uint64_t t0 = ts_begin();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const uint64_t t1 = ts_end();
    const auto s1 = std::chrono::steady_clock::now();
    const auto ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(s1 - s0).count();
    return t1 > t0 ? static_cast<double>(ns) / static_cast<double>(t1 - t0)
                   : 1.0;
}

inline void pin_this_thread(int cpu) {
#if defined(__linux__)
    if (cpu < 0) {
        return;
    }
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(static_cast<unsigned>(cpu) % CPU_SETSIZE, &set);
    pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
#else
    (void)cpu;
#endif
}

inline uint64_t pct(const std::vector<uint64_t>& sorted, double q) {
    if (sorted.empty()) {
        return 0;
    }
    const double pos = q * static_cast<double>(sorted.size() - 1);
    return sorted[static_cast<std::size_t>(pos + 0.5)];
}

struct Stats {
    uint64_t p50, p99, p999, max;
    double mean;
};

inline Stats summarize(std::vector<uint64_t>& ticks, double ns_per_tick) {
    std::vector<uint64_t> ns(ticks.size());
    for (std::size_t i = 0; i < ticks.size(); ++i) {
        ns[i] = static_cast<uint64_t>(static_cast<double>(ticks[i]) *
                                      ns_per_tick);
    }
    std::sort(ns.begin(), ns.end());
    double mean = 0;
    for (const uint64_t v : ns) {
        mean += static_cast<double>(v);
    }
    mean /= ns.empty() ? 1 : static_cast<double>(ns.size());
    return {pct(ns, 0.50), pct(ns, 0.99), pct(ns, 0.999),
            ns.empty() ? 0 : ns.back(), mean};
}

} // namespace steroids_bench
