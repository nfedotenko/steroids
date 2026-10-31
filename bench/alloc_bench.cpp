/*
 * Copyright(c) 2026 Nikolai Fedotenko.
 * Distributed under the MIT License (http://opensource.org/licenses/MIT)
 *
 * pool_allocator vs operator new (the system malloc) vs
 * std::pmr::unsynchronized_pool_resource.
 *
 * Methodology: single thread (matching pool_allocator's v1 contract),
 * serialized tick reads around each call, allocate and deallocate timed
 * separately, percentiles over all samples, raw CSV on demand. Patterns:
 *   - fixed-size churn at depth D: keep D blocks alive in a ring, free the
 *     oldest, allocate a new one (D=1 is pure LIFO reuse, D=64 forces the
 *     free list deeper);
 *   - mixed-size churn at depth D over a bag of odd sizes.
 * Publishable numbers come from a quiet Linux box (performance governor,
 * pinned core); anything else is a smoke run.
 */

#include "bench_util.h"

#include <steroids/memory/pool_allocator.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory_resource>
#include <new>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace steroids_bench;

//------------------------------------------------------------------------------
// Allocator adapters
//------------------------------------------------------------------------------
struct pool_adapter {
    static constexpr std::string_view name = "steroids_pool";
    void* allocate(std::size_t size) { return steroids::pool_allocate(size); }
    void deallocate(void* p, std::size_t size) noexcept {
        steroids::pool_deallocate(p, size);
    }
};

struct new_adapter {
    static constexpr std::string_view name = "operator_new";
    void* allocate(std::size_t size) { return ::operator new(size); }
    void deallocate(void* p, std::size_t size) noexcept {
        ::operator delete(p, size);
    }
};

struct pmr_pool_adapter {
    static constexpr std::string_view name = "pmr_unsync_pool";
    std::pmr::unsynchronized_pool_resource res;
    void* allocate(std::size_t size) { return res.allocate(size); }
    void deallocate(void* p, std::size_t size) noexcept {
        res.deallocate(p, size);
    }
};

//------------------------------------------------------------------------------
// Patterns
//------------------------------------------------------------------------------
struct Options {
    std::size_t samples = 100'000;
    std::size_t warmup = 10'000;
    int cpu = -1;
    std::string summary_csv;
};

// Keep `depth` blocks alive in a ring; each step frees the oldest and
// allocates a replacement, timing both calls separately.
template <class Alloc>
void run_churn(Alloc& alloc, const Options& opt, std::string_view pattern,
               const std::vector<std::size_t>& sizes, std::size_t depth,
               double ns_per_tick) {
    struct slot {
        void* p;
        std::size_t size;
    };
    std::vector<slot> ring(depth, {nullptr, 0});

    auto step_size = [&](std::size_t i) {
        return sizes[i % sizes.size()];
    };

    // Prefill + warmup
    for (std::size_t i = 0; i < depth; ++i) {
        const std::size_t s = step_size(i);
        ring[i] = {alloc.allocate(s), s};
        std::memset(ring[i].p, 0x5A, 1);
    }
    for (std::size_t i = 0; i < opt.warmup; ++i) {
        slot& sl = ring[i % depth];
        alloc.deallocate(sl.p, sl.size);
        const std::size_t s = step_size(i);
        sl = {alloc.allocate(s), s};
        std::memset(sl.p, 0x5A, 1);
    }

    std::vector<uint64_t> alloc_ticks(opt.samples);
    std::vector<uint64_t> free_ticks(opt.samples);

    for (std::size_t i = 0; i < opt.samples; ++i) {
        slot& sl = ring[i % depth];
        const uint64_t f0 = ts_begin();
        alloc.deallocate(sl.p, sl.size);
        const uint64_t f1 = ts_end();
        free_ticks[i] = f1 - f0;

        const std::size_t s = step_size(i);
        const uint64_t a0 = ts_begin();
        void* p = alloc.allocate(s);
        const uint64_t a1 = ts_end();
        alloc_ticks[i] = a1 - a0;

        std::memset(p, 0x5A, 1);
        sl = {p, s};
    }

    for (slot& sl : ring) {
        alloc.deallocate(sl.p, sl.size);
    }

    const Stats a = summarize(alloc_ticks, ns_per_tick);
    const Stats f = summarize(free_ticks, ns_per_tick);

    std::printf("%-15s %-10s depth=%-4zu  alloc p50=%llu p99=%llu "
                "p99.9=%llu max=%llu mean=%.1f | free p50=%llu p99=%llu "
                "p99.9=%llu max=%llu mean=%.1f (ns)\n",
                std::string(Alloc::name).c_str(),
                std::string(pattern).c_str(), depth,
                static_cast<unsigned long long>(a.p50),
                static_cast<unsigned long long>(a.p99),
                static_cast<unsigned long long>(a.p999),
                static_cast<unsigned long long>(a.max), a.mean,
                static_cast<unsigned long long>(f.p50),
                static_cast<unsigned long long>(f.p99),
                static_cast<unsigned long long>(f.p999),
                static_cast<unsigned long long>(f.max), f.mean);

    if (!opt.summary_csv.empty()) {
        std::ofstream csv(opt.summary_csv, std::ios::app);
        csv << Alloc::name << ',' << pattern << ',' << depth << ','
            << opt.samples << ',' << a.p50 << ',' << a.p99 << ',' << a.p999
            << ',' << a.max << ',' << a.mean << ',' << f.p50 << ',' << f.p99
            << ',' << f.p999 << ',' << f.max << ',' << f.mean << '\n';
    }
}

template <class Alloc>
void run_all(const Options& opt, double ns_per_tick) {
    Alloc alloc;
    const std::vector<std::size_t> mixed = {48,  96,   160,  288,
                                            544, 1056, 2080, 4000};
    for (std::size_t size : {std::size_t{64}, std::size_t{512},
                             std::size_t{4096}}) {
        const std::string pattern = "fixed" + std::to_string(size);
        run_churn(alloc, opt, pattern, {size}, 1, ns_per_tick);
        run_churn(alloc, opt, pattern, {size}, 64, ns_per_tick);
    }
    run_churn(alloc, opt, "mixed", mixed, 64, ns_per_tick);
}

bool parse_size(std::string_view s, std::size_t& out) {
    std::size_t v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') {
            return false;
        }
        v = v * 10 + static_cast<std::size_t>(c - '0');
    }
    out = v;
    return !s.empty();
}

} // namespace

int main(int argc, char** argv) {
    Options opt;
    std::string only = "all";
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        auto next = [&]() -> std::string_view {
            return i + 1 < argc ? std::string_view(argv[++i])
                                : std::string_view{};
        };
        if (a == "--samples") {
            parse_size(next(), opt.samples);
        } else if (a == "--warmup") {
            parse_size(next(), opt.warmup);
        } else if (a == "--cpu") {
            std::size_t c = 0;
            parse_size(next(), c);
            opt.cpu = static_cast<int>(c);
        } else if (a == "--summary-csv") {
            opt.summary_csv = next();
        } else if (a == "--allocator") {
            only = next();
        } else {
            std::fprintf(stderr,
                         "usage: %s [--allocator steroids_pool|operator_new|"
                         "pmr_unsync_pool|all] [--samples N] [--warmup N]"
                         " [--cpu N] [--summary-csv PATH]\n",
                         argv[0]);
            return 2;
        }
    }

    pin_this_thread(opt.cpu);
    const double ns_per_tick = measure_ns_per_tick();

    if (!opt.summary_csv.empty()) {
        std::ofstream csv(opt.summary_csv, std::ios::app);
        csv << "allocator,pattern,depth,samples,alloc_p50,alloc_p99,"
               "alloc_p99.9,alloc_max,alloc_mean,free_p50,free_p99,"
               "free_p99.9,free_max,free_mean\n";
    }

    if (only == "all" || only == "steroids_pool") {
        run_all<pool_adapter>(opt, ns_per_tick);
    }
    if (only == "all" || only == "operator_new") {
        run_all<new_adapter>(opt, ns_per_tick);
    }
    if (only == "all" || only == "pmr_unsync_pool") {
        run_all<pmr_pool_adapter>(opt, ns_per_tick);
    }
    return 0;
}
