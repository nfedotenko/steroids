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

inline constexpr std::size_t cacheline_size = 64;

// Bounded single-producer single-consumer queue of T.
//
// The classic two-index ring with cached counterparts: the producer reloads
// the consumer's index only when the ring looks full, the consumer reloads
// the producer's only when it looks empty, so in steady state each side
// runs on its own cache line and the only traffic between them is the
// release/acquire pair on the index it publishes.
//
// Capacity must be a power of two; one slot is sacrificed to distinguish
// full from empty.
template <typename T, std::size_t Capacity>
    requires(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0)
class spsc_queue {
    static constexpr std::size_t mask = Capacity - 1;

public:
    spsc_queue() noexcept
        : tail_(0), head_cache_(0), head_(0), tail_cache_(0) {}

    spsc_queue(const spsc_queue&) = delete;
    spsc_queue& operator=(const spsc_queue&) = delete;
    spsc_queue(spsc_queue&&) = delete;
    spsc_queue& operator=(spsc_queue&&) = delete;

    ~spsc_queue() { clear(); }

    // Producer. Returns false when the queue is full.
    bool enqueue(const T& item) noexcept(
        std::is_nothrow_copy_constructible_v<T>) {
        return emplace(item);
    }

    bool enqueue(T&& item) noexcept(
        std::is_nothrow_move_constructible_v<T>) {
        return emplace(std::move(item));
    }

    template <class... Args>
    bool emplace(Args&&... args) noexcept(
        std::is_nothrow_constructible_v<T, Args...>) {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        const std::size_t next = (tail + 1) & mask;

        if (next == head_cache_) {
            head_cache_ = head_.load(std::memory_order_acquire);
            if (next == head_cache_) {
                return false; // full
            }
        }

        ::new (static_cast<void*>(buf_[tail].bytes))
            T(std::forward<Args>(args)...);
        tail_.store(next, std::memory_order_release);
        return true;
    }

    // Consumer. Returns false when the queue is empty.
    bool dequeue(T& out) noexcept(
        std::is_nothrow_move_assignable_v<T>) {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        if (head == tail_cache_) {
            tail_cache_ = tail_.load(std::memory_order_acquire);
            if (head == tail_cache_) {
                return false; // empty
            }
        }

        T* ptr = std::launder(reinterpret_cast<T*>(buf_[head].bytes));
        out = std::move(*ptr);
        ptr->~T();

        head_.store((head + 1) & mask, std::memory_order_release);
        return true;
    }

    // Consumer. Drains and destroys the remaining elements.
    void clear() noexcept {
        const std::size_t tail = tail_.load(std::memory_order_acquire);
        std::size_t head = head_.load(std::memory_order_relaxed);
        while (head != tail) {
            T* ptr = std::launder(reinterpret_cast<T*>(buf_[head].bytes));
            ptr->~T();
            head = (head + 1) & mask;
        }
        head_.store(head, std::memory_order_release);
    }

    // Exact only when called from the consumer thread; approximate otherwise.
    std::size_t approx_size() const noexcept {
        const std::size_t h = head_.load(std::memory_order_acquire);
        const std::size_t t = tail_.load(std::memory_order_acquire);
        return (t + Capacity - h) & mask;
    }

    static constexpr std::size_t capacity() noexcept { return Capacity - 1; }

private:
    // Producer-owned line, then consumer-owned line: two lines of traffic
    // total, and no false sharing between the sides.
    alignas(cacheline_size) std::atomic<std::size_t> tail_;
    std::size_t head_cache_;
    alignas(cacheline_size) std::atomic<std::size_t> head_;
    std::size_t tail_cache_;

    struct cell {
        alignas(T) std::byte bytes[sizeof(T)];
    };
    alignas(cacheline_size) cell buf_[Capacity];
};

} // namespace steroids
