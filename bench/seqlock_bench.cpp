/*
 * Copyright(c) 2026 Nikolai Fedotenko.
 * Distributed under the MIT License (http://opensource.org/licenses/MIT)
 *
 * seqlock vs a mutex-protected value vs spin_lock-protected value, for the
 * "one writer publishes, readers snapshot" pattern. The writer updates at a
 * fixed pace on its own thread; we measure reader-side load latency
 * percentiles. A 48-byte L1-tick-like payload.
 */

#include "bench_util.h"

#include <steroids/sync/seqlock.h>
#include <steroids/sync/spin_lock.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using namespace steroids_bench;

struct Tick {
    std::uint64_t bid_px;
    std::uint64_t ask_px;
    std::uint64_t bid_qty;
    std::uint64_t ask_qty;
    std::uint64_t ts;
    std::uint64_t seq;
};

//------------------------------------------------------------------------------
// Contenders share this interface: writer-side publish, reader-side snapshot
//------------------------------------------------------------------------------
struct seqlock_holder {
    static constexpr std::string_view name = "steroids_seqlock";
    steroids::seqlock<Tick> cell;
    void publish(const Tick& t) noexcept { cell.store(t); }
    Tick snapshot() const noexcept { return cell.load(); }
};

struct mutex_holder {
    static constexpr std::string_view name = "std_mutex";
    mutable std::mutex m;
    Tick value{};
    void publish(const Tick& t) noexcept {
        const std::lock_guard<std::mutex> g(m);
        value = t;
    }
    Tick snapshot() const noexcept {
        const std::lock_guard<std::mutex> g(m);
        return value;
    }
};

struct spin_holder {
    static constexpr std::string_view name = "steroids_spin";
    mutable steroids::spin_lock m;
    Tick value{};
    void publish(const Tick& t) noexcept {
        const std::lock_guard<steroids::spin_lock> g(m);
        value = t;
    }
    Tick snapshot() const noexcept {
        const std::lock_guard<steroids::spin_lock> g(m);
        return value;
    }
};

struct Options {
    std::size_t samples = 200'000;
    std::size_t warmup = 20'000;
    std::size_t write_gap_ns = 1'000; // ~1M updates/s background writer
    int cpu_reader = -1;
    int cpu_writer = -1;
    std::string summary_csv;
};

template <class Holder>
void run(const Options& opt, double ns_per_tick) {
    Holder h;
    h.publish({1, 2, 3, 4, 5, 0});

    std::atomic<bool> stop{false};
    std::thread writer([&] {
        pin_this_thread(opt.cpu_writer);
        const auto gap_ticks = static_cast<uint64_t>(
            static_cast<double>(opt.write_gap_ns) / ns_per_tick);
        Tick t{100, 101, 7, 9, 0, 0};
        while (!stop.load(std::memory_order_relaxed)) {
            ++t.seq;
            t.ts = t.seq;
            h.publish(t);
            const uint64_t until = ts_end() + gap_ticks;
            while (ts_end() < until) {
            }
        }
    });

    pin_this_thread(opt.cpu_reader);
    std::uint64_t sink = 0;
    for (std::size_t i = 0; i < opt.warmup; ++i) {
        sink += h.snapshot().seq;
    }

    std::vector<uint64_t> ticks(opt.samples);
    for (std::size_t i = 0; i < opt.samples; ++i) {
        const uint64_t t0 = ts_begin();
        const Tick t = h.snapshot();
        const uint64_t t1 = ts_end();
        ticks[i] = t1 - t0;
        sink += t.seq;
    }
    stop.store(true, std::memory_order_relaxed);
    writer.join();
    if (sink == 42) {
        std::puts("");
    }

    const Stats s = summarize(ticks, ns_per_tick);
    std::printf("%-17s reader-load p50=%llu p99=%llu p99.9=%llu max=%llu "
                "mean=%.1f (ns)\n",
                std::string(Holder::name).c_str(),
                static_cast<unsigned long long>(s.p50),
                static_cast<unsigned long long>(s.p99),
                static_cast<unsigned long long>(s.p999),
                static_cast<unsigned long long>(s.max), s.mean);
    if (!opt.summary_csv.empty()) {
        std::ofstream csv(opt.summary_csv, std::ios::app);
        csv << Holder::name << ',' << opt.samples << ',' << s.p50 << ','
            << s.p99 << ',' << s.p999 << ',' << s.max << ',' << s.mean
            << '\n';
    }
}

bool parse_size(std::string_view s, std::size_t& out) {
    std::size_t v = 0;
    for (const char c : s) {
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
        } else if (a == "--write-gap-ns") {
            parse_size(next(), opt.write_gap_ns);
        } else if (a == "--cpu-reader") {
            std::size_t c = 0;
            parse_size(next(), c);
            opt.cpu_reader = static_cast<int>(c);
        } else if (a == "--cpu-writer") {
            std::size_t c = 0;
            parse_size(next(), c);
            opt.cpu_writer = static_cast<int>(c);
        } else if (a == "--summary-csv") {
            opt.summary_csv = next();
        } else {
            std::fprintf(stderr,
                         "usage: %s [--samples N] [--warmup N]"
                         " [--write-gap-ns N] [--cpu-reader N]"
                         " [--cpu-writer N] [--summary-csv PATH]\n",
                         argv[0]);
            return 2;
        }
    }

    const double ns_per_tick = measure_ns_per_tick();
    if (!opt.summary_csv.empty()) {
        std::ofstream csv(opt.summary_csv, std::ios::app);
        csv << "holder,samples,p50,p99,p99.9,max,mean\n";
    }

    run<seqlock_holder>(opt, ns_per_tick);
    run<mutex_holder>(opt, ns_per_tick);
    run<spin_holder>(opt, ns_per_tick);
    return 0;
}
