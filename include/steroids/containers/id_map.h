/*
 * Copyright(c) 2026 Nikolai Fedotenko.
 * Distributed under the MIT License (http://opensource.org/licenses/MIT)
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>

namespace steroids {

// constexpr FNV-1a: the usual companion of id_map — hash a string literal
// into a key at compile time (as steroidslog does for format strings).
constexpr std::uint32_t fnv1a_32(std::string_view s) noexcept {
    std::uint32_t h = 2166136261u;
    for (const char c : s) {
        h ^= static_cast<unsigned char>(c);
        h *= 16777619u;
    }
    return h;
}

template <std::size_t N>
constexpr std::uint32_t fnv1a_32(const char (&s)[N]) noexcept {
    return fnv1a_32(std::string_view{s, N - 1}); // strip trailing '\0'
}

//------------------------------------------------------------------------------
// Fixed-capacity lock-free map from non-zero 32-bit ids to small trivially
// copyable values (extracted from steroidslog's format-string registry).
//
// Open addressing with linear probing over atomics: readers are wait-free
// (no CAS, no locks), writers claim a slot with one CAS. Ids register once
// and are never erased — the intended use is interning: compile-time-hashed
// ids mapping to pointers/handles, registered lazily from any thread, read
// from hot paths.
//
// Same-id writes race benignly (last write wins); id 0 is reserved as
// "empty". put() returns false once the table is full — size it generously,
// probing degrades near full load. A get() racing the very first put() of
// an id may observe the slot claimed but the value not yet published and
// return T{} — callers treat T{} like "not there yet", as steroidslog does
// with its empty-format fallback.
//------------------------------------------------------------------------------
template <typename T, std::size_t Capacity>
    requires(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0 &&
             std::is_trivially_copyable_v<T> &&
             std::atomic<T>::is_always_lock_free)
class id_map {
    static constexpr std::size_t mask = Capacity - 1;

public:
    id_map() noexcept = default;
    id_map(const id_map&) = delete;
    id_map& operator=(const id_map&) = delete;

    // Any thread. Registers or overwrites the value for a non-zero id.
    bool put(std::uint32_t id, T value) noexcept {
        if (id == 0) {
            return false;
        }
        for (std::size_t probe = 0; probe < Capacity; ++probe) {
            slot& s = slots_[(id + probe) & mask];
            std::uint32_t expected = 0;
            if (s.key.compare_exchange_strong(expected, id,
                                              std::memory_order_acq_rel,
                                              std::memory_order_acquire) ||
                expected == id) {
                s.value.store(value, std::memory_order_release);
                return true;
            }
        }
        return false; // full
    }

    // Any thread, wait-free. Returns fallback when the id is unknown.
    T get(std::uint32_t id, T fallback = T{}) const noexcept {
        if (id == 0) {
            return fallback;
        }
        for (std::size_t probe = 0; probe < Capacity; ++probe) {
            const slot& s = slots_[(id + probe) & mask];
            const std::uint32_t k = s.key.load(std::memory_order_acquire);
            if (k == 0) {
                return fallback; // never registered
            }
            if (k == id) {
                return s.value.load(std::memory_order_acquire);
            }
        }
        return fallback;
    }

    bool contains(std::uint32_t id) const noexcept {
        if (id == 0) {
            return false;
        }
        for (std::size_t probe = 0; probe < Capacity; ++probe) {
            const slot& s = slots_[(id + probe) & mask];
            const std::uint32_t k = s.key.load(std::memory_order_acquire);
            if (k == 0) {
                return false;
            }
            if (k == id) {
                return true;
            }
        }
        return false;
    }

    static constexpr std::size_t capacity() noexcept { return Capacity; }

private:
    struct slot {
        std::atomic<std::uint32_t> key{0};
        std::atomic<T> value{};
    };

    slot slots_[Capacity];
};

} // namespace steroids
