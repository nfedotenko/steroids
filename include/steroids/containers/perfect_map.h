/*
 * Copyright(c) 2025 Nikolai Fedotenko.
 * Distributed under the MIT License (http://opensource.org/licenses/MIT)
 */

#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

#if defined(__x86_64__) || defined(_M_X64)
#define STEROIDS_PERFECT_MAP_SSE2 1
#include <emmintrin.h>
#elif defined(__aarch64__) || defined(_M_ARM64)
#define STEROIDS_PERFECT_MAP_NEON 1
#include <arm_neon.h>
#endif

#if defined(__SANITIZE_ADDRESS__)
#define STEROIDS_PERFECT_MAP_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define STEROIDS_PERFECT_MAP_ASAN 1
#endif
#endif

namespace steroids {

//------------------------------------------------------------------------------
// Compile-time perfect-hash map for a fixed set of short string keys
// (after GNU gperf and Lemire & Thiesen, "Billion-query-per-second lookup
// tables", 2026). The whole construction runs in constexpr:
//
//  - slot = (length + asso0[key[p0]] + asso1[key[p1]] + ...) & mask, with
//    up to three character positions picked greedily so that signatures
//    are unique. Unlike classic gperf, each position gets its own
//    association table, so keys that are permutations of one another
//    ("AB"/"BA") no longer collide structurally. Values are assigned by
//    conflict-driven bumping: find a colliding pair, bump the table entry
//    at the first position where they differ, repeat until perfect;
//  - keys are stored zero-padded to 16 bytes, so the runtime lookup is a
//    4-ish instruction hash plus ONE 16-byte vector compare (SSE2/NEON)
//    against the slot, masked by length via a sliding FF/00 table;
//  - the wide load may read up to 15 bytes past the probed key. That is
//    fine on real hardware within a page, so lookup() falls back to a
//    byte-wise compare only when the key sits within 16 bytes of a page
//    end — one almost-always-predicted branch (and always under ASan,
//    whose redzones legitimately object to over-reads).
//
// Constraints: keys are unique, non-empty, at most 16 bytes; the key set
// is fixed at compile time. get() is fully constexpr and used by the
// builder's self-check; lookup() is the fast path for runtime data.
//------------------------------------------------------------------------------

inline constexpr std::size_t perfect_map_max_key_len = 16;

template <class V>
struct perfect_kv {
    std::string_view key;
    V value;
};

template <class V, std::size_t TableSize>
class perfect_map {
    static_assert(std::has_single_bit(TableSize));

public:
    using kv = perfect_kv<V>;

    // Builder: throws in constexpr (= compile error) when the set cannot
    // be hashed perfectly at this table size — retry with a bigger one.
    template <std::size_t N>
    explicit constexpr perfect_map(const std::array<kv, N>& items) {
        static_assert(N >= 1 && N <= TableSize);
        for (const kv& item : items) {
            if (item.key.empty() ||
                item.key.size() > perfect_map_max_key_len) {
                throw "perfect_map: key must be 1..16 bytes";
            }
        }

        pick_positions(items);
        assign_associations(items);

        for (const kv& item : items) {
            slot_t& s = slots_[slot_of(item.key)];
            if (s.used) {
                throw "perfect_map: internal collision"; // defensive
            }
            s.used = true;
            s.len = static_cast<std::uint8_t>(item.key.size());
            for (std::size_t i = 0; i < item.key.size(); ++i) {
                s.key[i] = item.key[i];
            }
            s.value = item.value;
        }
    }

    // Constexpr-friendly scalar lookup.
    constexpr V get(std::string_view key, V fallback) const {
        if (key.empty() || key.size() > perfect_map_max_key_len) {
            return fallback;
        }
        const slot_t& s = slots_[slot_of(key)];
        if (!s.used || s.len != key.size()) {
            return fallback;
        }
        for (std::size_t i = 0; i < key.size(); ++i) {
            if (s.key[i] != key[i]) {
                return fallback;
            }
        }
        return s.value;
    }

    constexpr bool contains(std::string_view key) const {
        bool hit = false;
        if (!key.empty() && key.size() <= perfect_map_max_key_len) {
            const slot_t& s = slots_[slot_of(key)];
            hit = s.used && s.len == key.size();
            for (std::size_t i = 0; hit && i < key.size(); ++i) {
                hit = s.key[i] == key[i];
            }
        }
        return hit;
    }

    // Fast runtime lookup: one wide compare when the probed bytes cannot
    // cross into an unmapped page.
    V lookup(const char* data, std::size_t len, V fallback) const noexcept {
#if (defined(STEROIDS_PERFECT_MAP_SSE2) ||                                     \
     defined(STEROIDS_PERFECT_MAP_NEON)) &&                                    \
    !defined(STEROIDS_PERFECT_MAP_ASAN)
        if (len == 0 || len > perfect_map_max_key_len) {
            return fallback;
        }
        constexpr std::uintptr_t page = 4096;
        if ((reinterpret_cast<std::uintptr_t>(data) & (page - 1)) >
            page - perfect_map_max_key_len) [[unlikely]] {
            return get(std::string_view{data, len}, fallback);
        }
        const slot_t& s = slots_[slot_of_raw(data, len)];
        if (!s.used || s.len != len) {
            return fallback;
        }
        return wide_equal(data, len, s) ? s.value : fallback;
#else
        return get(std::string_view{data, len}, fallback);
#endif
    }

    V lookup(std::string_view key, V fallback) const noexcept {
        return lookup(key.data(), key.size(), fallback);
    }

    static constexpr std::size_t table_size() noexcept { return TableSize; }

private:
    struct slot_t {
        alignas(perfect_map_max_key_len)
            std::array<char, perfect_map_max_key_len> key{};
        std::uint8_t len{0};
        bool used{false};
        V value{};
    };

    static constexpr std::size_t max_positions = 3;

    static constexpr unsigned char char_at(std::string_view key,
                                           std::size_t pos) noexcept {
        return pos < key.size() ? static_cast<unsigned char>(key[pos])
                                : static_cast<unsigned char>(0);
    }

    constexpr std::size_t slot_of(std::string_view key) const {
        std::size_t h = key.size();
        for (std::size_t i = 0; i < pos_count_; ++i) {
            h += asso_[i][char_at(key, positions_[i])];
        }
        return h & (TableSize - 1);
    }

    std::size_t slot_of_raw(const char* data, std::size_t len) const noexcept {
        std::size_t h = len;
        for (std::size_t i = 0; i < pos_count_; ++i) {
            const std::size_t p = positions_[i];
            h += asso_[i][p < len ? static_cast<unsigned char>(data[p]) : 0u];
        }
        return h & (TableSize - 1);
    }

    // Signature = (length, chars at chosen positions); keys must end up
    // with unique signatures, else no association values can separate them.
    template <std::size_t N>
    constexpr void pick_positions(const std::array<kv, N>& items) {
        pos_count_ = 0;
        while (duplicate_pairs(items) != 0) {
            if (pos_count_ == max_positions) {
                throw "perfect_map: keys not separable by 3 positions + "
                      "length";
            }
            std::size_t best_pos = 0;
            std::size_t best_dups = static_cast<std::size_t>(-1);
            for (std::size_t cand = 0; cand < perfect_map_max_key_len;
                 ++cand) {
                positions_[pos_count_] = cand;
                ++pos_count_;
                const std::size_t dups = duplicate_pairs(items);
                --pos_count_;
                if (dups < best_dups) {
                    best_dups = dups;
                    best_pos = cand;
                }
            }
            positions_[pos_count_] = best_pos;
            ++pos_count_;
        }
    }

    template <std::size_t N>
    constexpr std::size_t
    duplicate_pairs(const std::array<kv, N>& items) const {
        std::size_t dups = 0;
        for (std::size_t a = 0; a < N; ++a) {
            for (std::size_t b = a + 1; b < N; ++b) {
                bool same = items[a].key.size() == items[b].key.size();
                for (std::size_t i = 0; same && i < pos_count_; ++i) {
                    same = char_at(items[a].key, positions_[i]) ==
                           char_at(items[b].key, positions_[i]);
                }
                dups += same ? 1 : 0;
            }
        }
        return dups;
    }

    // Conflict-driven assignment: while two keys share a slot, find the
    // first selected position where their characters differ (one exists —
    // signatures are unique — unless lengths differ, in which case slots
    // can still coincide via the tables; then any differing position
    // works too, and when ALL selected chars coincide the lengths alone
    // must separate them, so we bump a char of the second key anyway) and
    // bump that table entry. Bounded; failure means the table is too
    // small or the set too adversarial.
    template <std::size_t N>
    constexpr void assign_associations(const std::array<kv, N>& items) {
        const std::size_t budget = 128 * TableSize;
        for (std::size_t iter = 0; iter < budget; ++iter) {
            std::array<std::uint32_t, TableSize> taken{}; // key index + 1
            const kv* a = nullptr;
            const kv* b = nullptr;
            for (std::size_t k = 0; k < N && !b; ++k) {
                const std::size_t s = slot_of(items[k].key);
                if (taken[s] != 0) {
                    a = &items[taken[s] - 1];
                    b = &items[k];
                } else {
                    taken[s] = static_cast<std::uint32_t>(k + 1);
                }
            }
            if (!b) {
                return; // perfect
            }
            // Alternate which key moves and vary the step, otherwise two
            // key groups can chase each other around the table forever
            // with a constant offset.
            const kv* victim = (iter & 1) ? a : b;
            std::size_t bump_pos = 0;
            for (std::size_t i = 0; i < pos_count_; ++i) {
                if (char_at(a->key, positions_[i]) !=
                    char_at(b->key, positions_[i])) {
                    bump_pos = i;
                    break;
                }
            }
            const std::uint32_t step =
                1 + static_cast<std::uint32_t>(
                        (iter * 2654435761u >> 8) & 7u);
            asso_[bump_pos][char_at(victim->key, positions_[bump_pos])] +=
                step;
        }
        throw "perfect_map: no perfect hash found — use a larger TableSize";
    }

#if defined(STEROIDS_PERFECT_MAP_SSE2) && !defined(STEROIDS_PERFECT_MAP_ASAN)
    static bool wide_equal(const char* data, std::size_t len,
                           const slot_t& s) noexcept {
        const __m128i probe =
            _mm_loadu_si128(reinterpret_cast<const __m128i*>(data));
        const __m128i mask = _mm_loadu_si128(reinterpret_cast<const __m128i*>(
            len_masks() + perfect_map_max_key_len - len));
        const __m128i stored =
            _mm_load_si128(reinterpret_cast<const __m128i*>(s.key.data()));
        const __m128i eq =
            _mm_cmpeq_epi8(_mm_and_si128(probe, mask), stored);
        return _mm_movemask_epi8(eq) == 0xFFFF;
    }
#elif defined(STEROIDS_PERFECT_MAP_NEON) && !defined(STEROIDS_PERFECT_MAP_ASAN)
    static bool wide_equal(const char* data, std::size_t len,
                           const slot_t& s) noexcept {
        const uint8x16_t probe =
            vld1q_u8(reinterpret_cast<const std::uint8_t*>(data));
        const uint8x16_t mask = vld1q_u8(
            reinterpret_cast<const std::uint8_t*>(len_masks()) +
            perfect_map_max_key_len - len);
        const uint8x16_t stored =
            vld1q_u8(reinterpret_cast<const std::uint8_t*>(s.key.data()));
        const uint8x16_t eq = vceqq_u8(vandq_u8(probe, mask), stored);
        return vminvq_u8(eq) == 0xFF;
    }
#endif

#if (defined(STEROIDS_PERFECT_MAP_SSE2) ||                                     \
     defined(STEROIDS_PERFECT_MAP_NEON)) &&                                    \
    !defined(STEROIDS_PERFECT_MAP_ASAN)
    // 16 x FF then 16 x 00: loading at offset (16 - len) yields a mask
    // keeping exactly the first len bytes.
    static const unsigned char* len_masks() noexcept {
        alignas(32) static const unsigned char table[32] = {
            0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
            0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
            0,    0,    0,    0,    0,    0,    0,    0,
            0,    0,    0,    0,    0,    0,    0,    0};
        return table;
    }
#endif

    std::array<std::array<std::uint32_t, 256>, max_positions> asso_{};
    std::array<std::size_t, max_positions> positions_{};
    std::size_t pos_count_{0};
    std::array<slot_t, TableSize> slots_{};
};

// Builds a map with TableSize = bit_ceil(N) * Scale:
//   constexpr auto m = make_perfect_map<int>({{"GET", 1}, {"PUT", 2}});
template <class V, std::size_t Scale = 4, std::size_t N>
constexpr auto make_perfect_map(const perfect_kv<V> (&items)[N]) {
    using map_t = perfect_map<V, std::bit_ceil(N) * Scale>;
    std::array<perfect_kv<V>, N> arr{};
    for (std::size_t i = 0; i < N; ++i) {
        arr[i] = items[i];
    }
    return map_t(arr);
}

} // namespace steroids
