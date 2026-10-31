/*
 * Copyright(c) 2026 Nikolai Fedotenko.
 * Distributed under the MIT License (http://opensource.org/licenses/MIT)
 *
 * perfect_map vs std::unordered_map for a fixed key set (20 HTTP headers),
 * following the methodology of Lemire & Thiesen 2026: look keys up in
 * random order, report time per lookup; percentiles are taken over passes.
 * A miss-heavy variant probes keys that are not in the set.
 */

#include "bench_util.h"

#include <steroids/containers/perfect_map.h>

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace {

using namespace steroids_bench;

constexpr auto headers = steroids::make_perfect_map<int>({
    {"accept", 1},          {"accept-encoding", 2},
    {"accept-language", 3}, {"authorization", 4},
    {"cache-control", 5},   {"connection", 6},
    {"content-length", 7},  {"content-type", 8},
    {"cookie", 9},          {"date", 10},
    {"etag", 11},           {"expect", 12},
    {"host", 13},           {"origin", 14},
    {"pragma", 15},         {"referer", 16},
    {"user-agent", 17},     {"vary", 18},
    {"via", 19},            {"warning", 20},
});

const std::vector<std::string_view>& hit_keys() {
    static const std::vector<std::string_view> keys = {
        "accept",          "accept-encoding", "accept-language",
        "authorization",   "cache-control",   "connection",
        "content-length",  "content-type",    "cookie",
        "date",            "etag",            "expect",
        "host",            "origin",          "pragma",
        "referer",         "user-agent",      "vary",
        "via",             "warning"};
    return keys;
}

const std::vector<std::string_view>& miss_keys() {
    static const std::vector<std::string_view> keys = {
        "accept-charset", "x-forwarded-for", "upgrade",
        "te",             "trailer",         "server",
        "range",          "if-match",        "allow",
        "age",            "location",        "link",
        "forwarded",      "expires",         "digest",
        "depth",          "destination",     "cross-origin",
        "priority",       "warnings"};
    return keys;
}

// Deterministic shuffled probe sequence over the key list.
std::vector<std::string_view> probe_sequence(
    const std::vector<std::string_view>& keys, std::size_t length) {
    std::vector<std::string_view> probes(length);
    std::uint64_t x = 0x9E3779B97F4A7C15ull;
    for (std::size_t i = 0; i < length; ++i) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        probes[i] = keys[x % keys.size()];
    }
    return probes;
}

struct Options {
    std::size_t probes = 65'536;
    std::size_t passes = 300;
    int cpu = -1;
    std::string summary_csv;
};

template <class LookupFn>
void run(const Options& opt, std::string_view contender,
         std::string_view pattern, const std::vector<std::string_view>& seq,
         double ns_per_tick, LookupFn&& fn) {
    std::vector<uint64_t> ticks(opt.passes);
    std::int64_t checksum = 0;
    for (std::size_t p = 0; p < opt.passes; ++p) {
        const uint64_t t0 = ts_begin();
        std::int64_t sum = 0;
        for (const std::string_view key : seq) {
            sum += fn(key);
        }
        const uint64_t t1 = ts_end();
        ticks[p] = t1 - t0;
        checksum += sum;
    }
    if (checksum == 42) {
        std::puts("");
    }
    const Stats s = summarize(ticks, ns_per_tick);
    const auto per = static_cast<double>(seq.size());
    std::printf("%-15s %-6s p50=%.2f p99=%.2f max=%.2f mean=%.2f "
                "(ns/lookup)\n",
                std::string(contender).c_str(), std::string(pattern).c_str(),
                static_cast<double>(s.p50) / per,
                static_cast<double>(s.p99) / per,
                static_cast<double>(s.max) / per, s.mean / per);
    if (!opt.summary_csv.empty()) {
        std::ofstream csv(opt.summary_csv, std::ios::app);
        csv << contender << ',' << pattern << ','
            << static_cast<double>(s.p50) / per << ','
            << static_cast<double>(s.p99) / per << ','
            << static_cast<double>(s.max) / per << ',' << s.mean / per
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
        if (a == "--probes") {
            parse_size(next(), opt.probes);
        } else if (a == "--passes") {
            parse_size(next(), opt.passes);
        } else if (a == "--cpu") {
            std::size_t c = 0;
            parse_size(next(), c);
            opt.cpu = static_cast<int>(c);
        } else if (a == "--summary-csv") {
            opt.summary_csv = next();
        } else {
            std::fprintf(stderr,
                         "usage: %s [--probes N] [--passes N] [--cpu N]"
                         " [--summary-csv PATH]\n",
                         argv[0]);
            return 2;
        }
    }

    pin_this_thread(opt.cpu);
    const double ns_per_tick = measure_ns_per_tick();
    if (!opt.summary_csv.empty()) {
        std::ofstream csv(opt.summary_csv, std::ios::app);
        csv << "contender,pattern,p50,p99,max,mean\n";
    }

    std::unordered_map<std::string_view, int> umap;
    for (int i = 0; i < static_cast<int>(hit_keys().size()); ++i) {
        umap.emplace(hit_keys()[static_cast<std::size_t>(i)], i + 1);
    }

    const auto hits = probe_sequence(hit_keys(), opt.probes);
    const auto misses = probe_sequence(miss_keys(), opt.probes);

    for (const auto& [name, seq] :
         {std::pair<std::string_view,
                    const std::vector<std::string_view>*>{"hits", &hits},
          {"misses", &misses}}) {
        run(opt, "perfect_map", name, *seq, ns_per_tick,
            [](std::string_view k) { return headers.lookup(k, -1); });
        run(opt, "unordered_map", name, *seq, ns_per_tick,
            [&](std::string_view k) {
                const auto it = umap.find(k);
                return it != umap.end() ? it->second : -1;
            });
    }
    return 0;
}
