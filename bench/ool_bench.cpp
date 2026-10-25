/*
 * Copyright(c) 2025 Nikolai Fedotenko.
 * Distributed under the MIT License (http://opensource.org/licenses/MIT)
 *
 * out_of_line vs the original OutOfLine design (one global std::map behind a
 * mutex, after the 2018 Headlands Tech post) vs keeping cold fields inline.
 *
 * Three questions, measured separately:
 *  - hot iteration: sum a hot int over N objects — the locality win that
 *    motivates the pattern in the first place (inline is the baseline that
 *    drags cold bytes through the cache);
 *  - cold access: latency of reaching the cold part;
 *  - churn: construct+destroy cost (registry insert/erase vs none).
 */

#include "bench_util.h"

#include <steroids/memory/out_of_line.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace steroids_bench;

//------------------------------------------------------------------------------
// Cold payload: realistically bulky
//------------------------------------------------------------------------------
struct ColdBlob {
    std::string path{"/var/lib/service/sessions/0000000000"};
    std::string note{"created by bench"};
    char stats[104]{};
};

//------------------------------------------------------------------------------
// Contender 1: cold fields inline (the baseline the pattern fights)
//------------------------------------------------------------------------------
struct inline_obj {
    int hot{0};
    ColdBlob cold_data;
    ColdBlob& cold() noexcept { return cold_data; }
};

//------------------------------------------------------------------------------
// Contender 2: steroids::out_of_line (sharded registry)
//------------------------------------------------------------------------------
struct ool_obj : steroids::out_of_line<ool_obj, ColdBlob> {
    int hot{0};
};

//------------------------------------------------------------------------------
// Contender 3: the original design — one global std::map behind a mutex
//------------------------------------------------------------------------------
template <class Derived, class Cold>
class map_out_of_line {
public:
    map_out_of_line() {
        const std::lock_guard<std::mutex> g(mutex());
        registry()[this] = std::make_unique<Cold>();
    }
    map_out_of_line(const map_out_of_line& other) {
        const std::lock_guard<std::mutex> g(mutex());
        registry()[this] = std::make_unique<Cold>(*registry().at(&other));
    }
    map_out_of_line(map_out_of_line&& other) noexcept {
        const std::lock_guard<std::mutex> g(mutex());
        auto it = registry().find(&other);
        registry()[this] = std::move(it->second);
        registry().erase(it);
    }
    ~map_out_of_line() {
        const std::lock_guard<std::mutex> g(mutex());
        registry().erase(this);
    }
    Cold& cold() noexcept {
        const std::lock_guard<std::mutex> g(mutex());
        return *registry().at(this);
    }

private:
    using map_t = std::map<const void*, std::unique_ptr<Cold>>;
    static map_t& registry() {
        static map_t m;
        return m;
    }
    static std::mutex& mutex() {
        static std::mutex m;
        return m;
    }
};

struct map_obj : map_out_of_line<map_obj, ColdBlob> {
    int hot{0};
};

//------------------------------------------------------------------------------

struct Options {
    std::size_t objects = 65'536;
    std::size_t passes = 400;
    std::size_t accesses = 200'000;
    std::size_t churn = 100'000;
    int cpu = -1;
    std::string summary_csv;
};

void report(const Options& opt, std::string_view contender,
            std::string_view pattern, Stats s, double per_divisor = 1.0) {
    std::printf("%-13s %-12s p50=%llu p99=%llu p99.9=%llu max=%llu "
                "mean=%.1f (ns%s)\n",
                std::string(contender).c_str(), std::string(pattern).c_str(),
                static_cast<unsigned long long>(
                    static_cast<uint64_t>(s.p50 / per_divisor)),
                static_cast<unsigned long long>(
                    static_cast<uint64_t>(s.p99 / per_divisor)),
                static_cast<unsigned long long>(
                    static_cast<uint64_t>(s.p999 / per_divisor)),
                static_cast<unsigned long long>(
                    static_cast<uint64_t>(s.max / per_divisor)),
                s.mean / per_divisor, per_divisor > 1.0 ? "/obj" : "");
    if (!opt.summary_csv.empty()) {
        std::ofstream csv(opt.summary_csv, std::ios::app);
        csv << contender << ',' << pattern << ','
            << static_cast<uint64_t>(s.p50 / per_divisor) << ','
            << static_cast<uint64_t>(s.p99 / per_divisor) << ','
            << static_cast<uint64_t>(s.p999 / per_divisor) << ','
            << static_cast<uint64_t>(s.max / per_divisor) << ','
            << s.mean / per_divisor << '\n';
    }
}

// Hot iteration: per-pass time over N objects, reported per object.
template <class Obj>
void bench_hot_iteration(const Options& opt, std::string_view name,
                         double ns_per_tick) {
    std::vector<Obj> v(opt.objects);
    for (std::size_t i = 0; i < v.size(); ++i) {
        v[i].hot = static_cast<int>(i);
    }

    std::vector<uint64_t> ticks(opt.passes);
    std::int64_t checksum = 0;
    for (std::size_t p = 0; p < opt.passes; ++p) {
        const uint64_t t0 = ts_begin();
        std::int64_t sum = 0;
        for (const Obj& o : v) {
            sum += o.hot;
        }
        const uint64_t t1 = ts_end();
        ticks[p] = t1 - t0;
        checksum += sum;
    }
    if (checksum == 42) { // defeat the optimizer without volatile noise
        std::puts("");
    }
    Stats s = summarize(ticks, ns_per_tick);
    report(opt, name, "hot_iter", s, static_cast<double>(opt.objects));
}

// Cold access latency per call.
template <class Obj>
void bench_cold_access(const Options& opt, std::string_view name,
                       double ns_per_tick) {
    std::vector<Obj> v(4096);
    std::vector<uint64_t> ticks(opt.accesses);
    std::size_t sink = 0;
    for (std::size_t i = 0; i < opt.accesses; ++i) {
        Obj& o = v[(i * 2654435761u) & 4095];
        const uint64_t t0 = ts_begin();
        sink += o.cold().stats[0];
        const uint64_t t1 = ts_end();
        ticks[i] = t1 - t0;
    }
    if (sink == 42) {
        std::puts("");
    }
    Stats s = summarize(ticks, ns_per_tick);
    report(opt, name, "cold_access", s);
}

// Construct + destroy one object per sample.
template <class Obj>
void bench_churn(const Options& opt, std::string_view name,
                 double ns_per_tick) {
    std::vector<uint64_t> ticks(opt.churn);
    for (std::size_t i = 0; i < opt.churn; ++i) {
        const uint64_t t0 = ts_begin();
        {
            Obj o;
            o.hot = static_cast<int>(i);
        }
        const uint64_t t1 = ts_end();
        ticks[i] = t1 - t0;
    }
    Stats s = summarize(ticks, ns_per_tick);
    report(opt, name, "churn", s);
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
        if (a == "--objects") {
            parse_size(next(), opt.objects);
        } else if (a == "--passes") {
            parse_size(next(), opt.passes);
        } else if (a == "--accesses") {
            parse_size(next(), opt.accesses);
        } else if (a == "--churn") {
            parse_size(next(), opt.churn);
        } else if (a == "--cpu") {
            std::size_t c = 0;
            parse_size(next(), c);
            opt.cpu = static_cast<int>(c);
        } else if (a == "--summary-csv") {
            opt.summary_csv = next();
        } else {
            std::fprintf(stderr,
                         "usage: %s [--objects N] [--passes N] [--accesses N]"
                         " [--churn N] [--cpu N] [--summary-csv PATH]\n",
                         argv[0]);
            return 2;
        }
    }

    pin_this_thread(opt.cpu);
    const double ns_per_tick = measure_ns_per_tick();

    if (!opt.summary_csv.empty()) {
        std::ofstream csv(opt.summary_csv, std::ios::app);
        csv << "contender,pattern,p50,p99,p99.9,max,mean\n";
    }

    bench_hot_iteration<inline_obj>(opt, "inline", ns_per_tick);
    bench_hot_iteration<ool_obj>(opt, "steroids_ool", ns_per_tick);
    bench_hot_iteration<map_obj>(opt, "map_ool", ns_per_tick);

    bench_cold_access<inline_obj>(opt, "inline", ns_per_tick);
    bench_cold_access<ool_obj>(opt, "steroids_ool", ns_per_tick);
    bench_cold_access<map_obj>(opt, "map_ool", ns_per_tick);

    bench_churn<inline_obj>(opt, "inline", ns_per_tick);
    bench_churn<ool_obj>(opt, "steroids_ool", ns_per_tick);
    bench_churn<map_obj>(opt, "map_ool", ns_per_tick);

    return 0;
}
