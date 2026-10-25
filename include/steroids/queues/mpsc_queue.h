/*
 * Copyright(c) 2025 Nikolai Fedotenko.
 * Distributed under the MIT License (http://opensource.org/licenses/MIT)
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>

namespace steroids {

//------------------------------------------------------------------------------
// Bounded multi-producer single-consumer queue (Vyukov-style sequenced
// cells). Producers claim a slot with one CAS on the tail; the single
// consumer runs CAS-free on plain loads of its own index. Each cell carries
// a sequence number that both publishes the element and hands the slot back
// to producers, so no side carries a lock and a full/empty check never
// walks the buffer.
//
// FIFO per producer; elements from different producers interleave in claim
// order. Capacity must be a power of two.
//------------------------------------------------------------------------------
template <typename T, std::size_t Capacity>
    requires(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0)
class mpsc_queue {
    static constexpr std::size_t mask = Capacity - 1;
    static constexpr std::size_t cacheline = 64;

public:
    mpsc_queue() noexcept {
        for (std::size_t i = 0; i < Capacity; ++i) {
            cells_[i].seq.store(i, std::memory_order_relaxed);
        }
    }

    mpsc_queue(const mpsc_queue&) = delete;
    mpsc_queue& operator=(const mpsc_queue&) = delete;
    mpsc_queue(mpsc_queue&&) = delete;
    mpsc_queue& operator=(mpsc_queue&&) = delete;

    ~mpsc_queue() {
        // No concurrent access by now (contract): destroy what is left
        // without requiring T to be default-constructible.
        const std::size_t tail = tail_.load(std::memory_order_acquire);
        for (std::size_t pos = head_; pos != tail; ++pos) {
            cell& c = cells_[pos & mask];
            std::launder(reinterpret_cast<T*>(c.bytes))->~T();
        }
    }

    // Any thread. Returns false when the queue is full.
    template <class... Args>
    bool emplace(Args&&... args) noexcept(
        std::is_nothrow_constructible_v<T, Args...>) {
        cell* c;
        std::size_t pos = tail_.load(std::memory_order_relaxed);
        for (;;) {
            c = &cells_[pos & mask];
            const std::size_t seq = c->seq.load(std::memory_order_acquire);
            const auto dif = static_cast<std::ptrdiff_t>(seq) -
                             static_cast<std::ptrdiff_t>(pos);
            if (dif == 0) {
                if (tail_.compare_exchange_weak(pos, pos + 1,
                                                std::memory_order_relaxed)) {
                    break;
                }
            } else if (dif < 0) {
                return false; // full
            } else {
                pos = tail_.load(std::memory_order_relaxed);
            }
        }
        ::new (static_cast<void*>(c->bytes)) T(std::forward<Args>(args)...);
        c->seq.store(pos + 1, std::memory_order_release);
        return true;
    }

    bool enqueue(const T& v) noexcept(
        std::is_nothrow_copy_constructible_v<T>) {
        return emplace(v);
    }

    bool enqueue(T&& v) noexcept(std::is_nothrow_move_constructible_v<T>) {
        return emplace(std::move(v));
    }

    // Consumer thread only. Returns false when the queue is empty.
    bool dequeue(T& out) noexcept(std::is_nothrow_move_assignable_v<T>) {
        cell* c = &cells_[head_ & mask];
        const std::size_t seq = c->seq.load(std::memory_order_acquire);
        const auto dif = static_cast<std::ptrdiff_t>(seq) -
                         static_cast<std::ptrdiff_t>(head_ + 1);
        if (dif < 0) {
            return false; // empty (or a producer mid-write)
        }
        T* ptr = std::launder(reinterpret_cast<T*>(c->bytes));
        out = std::move(*ptr);
        ptr->~T();
        c->seq.store(head_ + Capacity, std::memory_order_release);
        ++head_;
        return true;
    }

    static constexpr std::size_t capacity() noexcept { return Capacity; }

private:
    struct cell {
        std::atomic<std::size_t> seq;
        alignas(T) std::byte bytes[sizeof(T)];
    };

    alignas(cacheline) std::atomic<std::size_t> tail_{0}; // producers
    alignas(cacheline) std::size_t head_{0};              // consumer-owned
    alignas(cacheline) cell cells_[Capacity];
};

} // namespace steroids
