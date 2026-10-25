/*
 * Copyright(c) 2025 Nikolai Fedotenko.
 * Distributed under the MIT License (http://opensource.org/licenses/MIT)
 */

#pragma once

#include <steroids/sync/spin_lock.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace steroids {

//------------------------------------------------------------------------------
// Hot/cold field splitting with RAII, after the OutOfLine pattern
// (Headlands Tech, 2018): the fast object keeps only its hot members, the
// cold members live elsewhere and follow the object's lifetime — including
// moves. Iterating a container of fast objects then touches only hot bytes.
//
// Where the original keeps every cold object in one global std::map, this
// registry is sharded 64 ways (pointer-hash → shard) with an open-addressing
// table and a spin lock per shard: cold() stays cheap under concurrency and
// unrelated objects do not contend.
//
// Usage:
//   struct Session : steroids::out_of_line<Session, SessionCold> {
//       int fd;   // hot
//   };
//   s.cold().path = ...;
//
// Notes:
//  - cold() and destruction are O(1) average via the registry; this is NOT
//    for the nanosecond-critical path itself — it is for keeping the hot
//    path's data compact.
//  - a moved-from object owns no cold data; calling cold() on it is UB.
//  - moves/destruction may run on any thread (the registry is
//    synchronized).
//------------------------------------------------------------------------------

template <class Derived, class Cold>
class out_of_line {
public:
    out_of_line() { registry().insert(this, std::make_unique<Cold>()); }

    explicit out_of_line(Cold cold) {
        registry().insert(this, std::make_unique<Cold>(std::move(cold)));
    }

    out_of_line(const out_of_line& other) {
        registry().insert(this,
                          std::make_unique<Cold>(*other.cold_ptr()));
    }

    out_of_line(out_of_line&& other) noexcept {
        registry().rekey(&other, this);
    }

    out_of_line& operator=(const out_of_line& other) {
        if (this != &other) {
            *cold_ptr() = *other.cold_ptr();
        }
        return *this;
    }

    out_of_line& operator=(out_of_line&& other) noexcept {
        if (this != &other) {
            registry().erase(this);
            registry().rekey(&other, this);
        }
        return *this;
    }

    ~out_of_line() { registry().erase(this); }

    Cold& cold() noexcept { return *cold_ptr(); }
    const Cold& cold() const noexcept { return *cold_ptr(); }

private:
    Cold* cold_ptr() const noexcept {
        return registry().find(this);
    }

    //--------------------------------------------------------------------------
    // Sharded pointer → unique_ptr<Cold> registry, one instance per
    // (Derived, Cold) pair.
    //--------------------------------------------------------------------------
    class cold_registry {
        struct shard {
            // Open addressing with linear probing and tombstones.
            struct entry {
                const void* key{nullptr};
                std::unique_ptr<Cold> value;
            };
            static constexpr const void* tombstone() noexcept {
                return reinterpret_cast<const void*>(std::uintptr_t{1});
            }

            spin_lock lock;
            std::vector<entry> table{std::vector<entry>(16)};
            std::size_t used{0};     // live + tombstones
            std::size_t live{0};

            static std::size_t slot(const void* key,
                                    std::size_t mask) noexcept {
                return hash_of(key) & mask;
            }

            void grow_if_needed() {
                if (used * 10 < table.size() * 7) {
                    return;
                }
                const std::size_t new_size =
                    live * 10 >= table.size() * 5 ? table.size() * 2
                                                  : table.size();
                std::vector<entry> next(new_size);
                const std::size_t mask = next.size() - 1;
                for (entry& e : next) {
                    e.key = nullptr;
                }
                for (entry& e : table) {
                    if (e.key && e.key != tombstone()) {
                        std::size_t i = slot(e.key, mask);
                        while (next[i].key) {
                            i = (i + 1) & mask;
                        }
                        next[i].key = e.key;
                        next[i].value = std::move(e.value);
                    }
                }
                table = std::move(next);
                used = live;
            }

            void insert(const void* key, std::unique_ptr<Cold> value) {
                grow_if_needed();
                const std::size_t mask = table.size() - 1;
                std::size_t i = slot(key, mask);
                while (table[i].key && table[i].key != tombstone()) {
                    i = (i + 1) & mask;
                }
                if (!table[i].key) {
                    ++used;
                }
                table[i].key = key;
                table[i].value = std::move(value);
                ++live;
            }

            entry* lookup(const void* key) noexcept {
                const std::size_t mask = table.size() - 1;
                std::size_t i = slot(key, mask);
                while (table[i].key) {
                    if (table[i].key == key) {
                        return &table[i];
                    }
                    i = (i + 1) & mask;
                }
                return nullptr;
            }
        };

    public:
        void insert(const void* key, std::unique_ptr<Cold> value) {
            shard& s = shard_of(key);
            std::lock_guard<spin_lock> guard(s.lock);
            s.insert(key, std::move(value));
        }

        Cold* find(const void* key) noexcept {
            shard& s = shard_of(key);
            std::lock_guard<spin_lock> guard(s.lock);
            auto* e = s.lookup(key);
            return e ? e->value.get() : nullptr;
        }

        void erase(const void* key) noexcept {
            shard& s = shard_of(key);
            std::unique_ptr<Cold> reaped; // destroy outside the lock
            {
                std::lock_guard<spin_lock> guard(s.lock);
                if (auto* e = s.lookup(key)) {
                    reaped = std::move(e->value);
                    e->key = shard::tombstone();
                    --s.live;
                }
            }
        }

        // Moves ownership of `from`'s cold object to the key `to`. The keys
        // may hash to different shards; lock ordering by address avoids
        // deadlock.
        void rekey(const void* from, const void* to) noexcept {
            shard& sf = shard_of(from);
            shard& st = shard_of(to);
            std::unique_ptr<Cold> value;
            if (&sf == &st) {
                std::lock_guard<spin_lock> guard(sf.lock);
                if (auto* e = sf.lookup(from)) {
                    value = std::move(e->value);
                    e->key = shard::tombstone();
                    --sf.live;
                }
                if (value) {
                    sf.insert(to, std::move(value));
                }
                return;
            }
            spin_lock& first = &sf < &st ? sf.lock : st.lock;
            spin_lock& second = &sf < &st ? st.lock : sf.lock;
            std::lock_guard<spin_lock> g1(first);
            std::lock_guard<spin_lock> g2(second);
            if (auto* e = sf.lookup(from)) {
                value = std::move(e->value);
                e->key = shard::tombstone();
                --sf.live;
            }
            if (value) {
                st.insert(to, std::move(value));
            }
        }

    private:
        static constexpr std::size_t shard_count = 64;

        static std::size_t hash_of(const void* p) noexcept {
            // Fibonacci hash over the address sans alignment bits.
            return (reinterpret_cast<std::uintptr_t>(p) >> 4) *
                   std::uintptr_t{11400714819323198485ull};
        }

        shard& shard_of(const void* p) noexcept {
            return shards_[hash_of(p) >> (sizeof(std::size_t) * 8 - 6)];
        }

        shard shards_[shard_count];
    };

    static cold_registry& registry() {
        static cold_registry instance;
        return instance;
    }
};

} // namespace steroids
