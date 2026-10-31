/*
 * Copyright(c) 2026 Nikolai Fedotenko.
 * Distributed under the MIT License (http://opensource.org/licenses/MIT)
 */

#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <string>
#include <string_view>

#if !defined(_WIN32)
#include <sys/mman.h>
#include <unistd.h>
#endif

#include <steroids/containers/perfect_map.h>

using steroids::make_perfect_map;

namespace {

constexpr auto methods = make_perfect_map<int>({
    {"GET", 1},
    {"PUT", 2},
    {"POST", 3},
    {"DELETE", 4},
    {"PATCH", 5},
    {"HEAD", 6},
    {"OPTIONS", 7},
});

constexpr auto headers = make_perfect_map<int>({
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

} // namespace

TEST(PerfectMap, ConstructionAndHitsAreConstexpr) {
    // The whole point: built and queried at compile time.
    static_assert(methods.get("GET", -1) == 1);
    static_assert(methods.get("DELETE", -1) == 4);
    static_assert(methods.get("TRACE", -1) == -1);
    static_assert(methods.contains("OPTIONS"));
    static_assert(!methods.contains("BREW"));
    SUCCEED();
}

TEST(PerfectMap, AllKeysHitAllMissesMiss) {
    const steroids::perfect_kv<int> all[] = {
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
    };
    for (const auto& [key, value] : all) {
        EXPECT_EQ(headers.get(key, -1), value) << key;
        EXPECT_EQ(headers.lookup(key, -1), value) << key;
    }
    for (std::string_view miss :
         {"accept-charset", "x-forwarded-for", "etags", "hos", "hostt", "",
          "a", "warning!", "this-key-is-way-over-sixteen-bytes"}) {
        EXPECT_EQ(headers.get(miss, -1), -1) << miss;
        EXPECT_EQ(headers.lookup(miss, -1), -1) << miss;
    }
}

TEST(PerfectMap, ScalarAndWideLookupAgreeOnMutatedKeys) {
    // Flip every byte of every key: lookup() (wide compare) and get()
    // (scalar) must agree everywhere.
    const char* keys[] = {"accept", "content-length", "via", "warning"};
    for (const char* k : keys) {
        std::string probe = k;
        for (std::size_t i = 0; i < probe.size(); ++i) {
            for (const char repl : {'\0', 'z', char(0xFF)}) {
                std::string mutated = probe;
                mutated[i] = repl;
                EXPECT_EQ(headers.lookup(mutated, -1),
                          headers.get(mutated, -1))
                    << k << " @" << i;
            }
        }
    }
}

#if !defined(_WIN32)
// A key placed flush against an unmapped page: the page guard must route
// the probe through the scalar path instead of reading into the guard.
TEST(PerfectMap, PageBoundaryGuard) {
    const long page = sysconf(_SC_PAGESIZE);
    ASSERT_GT(page, 0);
    char* base = static_cast<char*>(
        mmap(nullptr, static_cast<std::size_t>(page) * 2,
             PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    ASSERT_NE(base, MAP_FAILED);
    ASSERT_EQ(mprotect(base + page, static_cast<std::size_t>(page),
                       PROT_NONE),
              0);

    const std::string_view key = "content-length";
    char* at_edge = base + page - static_cast<long>(key.size());
    std::memcpy(at_edge, key.data(), key.size());
    EXPECT_EQ(headers.lookup(at_edge, key.size(), -1), 7);

    // And a miss at the same position.
    const std::string_view miss = "content-lengtX";
    std::memcpy(at_edge, miss.data(), miss.size());
    EXPECT_EQ(headers.lookup(at_edge, miss.size(), -1), -1);

    munmap(base, static_cast<std::size_t>(page) * 2);
}
#endif

TEST(PerfectMap, HundredTickers) {
    // 100 synthetic two-letter tickers built at runtime: the builder is
    // constexpr-capable but must work from runtime data too.
    static char names[100][3];
    std::array<steroids::perfect_kv<int>, 100> items{};
    for (int i = 0; i < 100; ++i) {
        names[i][0] = static_cast<char>('A' + i / 10);
        names[i][1] = static_cast<char>('A' + i % 10);
        names[i][2] = '\0';
        items[static_cast<std::size_t>(i)] = {
            std::string_view{names[i], 2}, i};
    }
    const steroids::perfect_map<int, 512> m(items);
    for (int i = 0; i < 100; ++i) {
        EXPECT_EQ(m.get(std::string_view{names[i], 2}, -1), i);
        EXPECT_EQ(m.lookup(std::string_view{names[i], 2}, -1), i);
    }
    EXPECT_EQ(m.get("ZZ", -1), -1);
    EXPECT_EQ(m.lookup(std::string_view{"KA"}, -1), -1);
}
