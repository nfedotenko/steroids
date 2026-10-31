/*
 * Copyright(c) 2026 Nikolai Fedotenko.
 * Distributed under the MIT License (http://opensource.org/licenses/MIT)
 */

#pragma once

#include <steroids/ipc/shared_memory.h>
#include <steroids/queues/spsc_queue.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace steroids {

//------------------------------------------------------------------------------
// SPSC queue across process boundaries: steroids::spsc_queue placed into a
// named shared-memory segment. The queue qualifies as-is — it holds only
// indices and an in-object buffer, no pointers — and the segment starts
// with a small validated header so a producer and a consumer built from
// different binaries fail loudly instead of corrupting each other:
// magic, layout version, element size/alignment and capacity must match.
//
// T must be trivially copyable (it crosses an address-space boundary) and
// lock-free-atomics must be address-free on the platform (they are on the
// mainstream ones; std::atomic<size_t>::is_always_lock_free is asserted).
//
// Lifecycle: create() constructs the queue and publishes it via a release
// store on the ready flag; open() validates and attaches. Nobody runs the
// queue's destructor — the memory outlives any single process and T is
// trivially destructible anyway; unlink() removes the name when done.
//------------------------------------------------------------------------------
template <typename T, std::size_t Capacity>
    requires(std::is_trivially_copyable_v<T> &&
             std::is_trivially_destructible_v<T>)
class shm_spsc_channel {
    static_assert(std::atomic<std::size_t>::is_always_lock_free);

    using queue_t = spsc_queue<T, Capacity>;

    struct header_t {
        std::uint64_t magic;
        std::uint32_t version;
        std::uint32_t elem_size;
        std::uint32_t elem_align;
        std::uint32_t pad_;
        std::uint64_t capacity;
        std::atomic<std::uint32_t> ready;
    };

    static constexpr std::uint64_t magic_v = 0x5354'4552'4F49'4453ull; // STEROIDS
    static constexpr std::uint32_t version_v = 1;
    static constexpr std::size_t queue_offset =
        (sizeof(header_t) + alignof(queue_t) - 1) & ~(alignof(queue_t) - 1);

public:
    static constexpr std::size_t segment_size =
        queue_offset + sizeof(queue_t);

    static shm_spsc_channel create(const std::string& name) {
        shared_memory shm = shared_memory::create(name, segment_size);
        auto* header = ::new (shm.data()) header_t{
            magic_v,
            version_v,
            static_cast<std::uint32_t>(sizeof(T)),
            static_cast<std::uint32_t>(alignof(T)),
            0,
            Capacity,
            {},
        };
        ::new (static_cast<char*>(shm.data()) + queue_offset) queue_t();
        header->ready.store(1, std::memory_order_release);
        return shm_spsc_channel(std::move(shm));
    }

    static shm_spsc_channel open(const std::string& name) {
        shared_memory shm = shared_memory::open(name, segment_size);
        const auto* header = static_cast<const header_t*>(shm.data());
        if (header->ready.load(std::memory_order_acquire) != 1) {
            throw std::runtime_error("shm_spsc_channel: not ready");
        }
        if (header->magic != magic_v || header->version != version_v) {
            throw std::runtime_error("shm_spsc_channel: wrong magic/version");
        }
        if (header->elem_size != sizeof(T) ||
            header->elem_align != alignof(T) ||
            header->capacity != Capacity) {
            throw std::runtime_error(
                "shm_spsc_channel: layout mismatch between sides");
        }
        return shm_spsc_channel(std::move(shm));
    }

    static void unlink(const std::string& name) noexcept {
        shared_memory::unlink(name);
    }

    // The producer side calls enqueue/emplace, the consumer side dequeue —
    // exactly one process each, per the queue's contract.
    queue_t& queue() noexcept {
        return *std::launder(reinterpret_cast<queue_t*>(
            static_cast<char*>(shm_.data()) + queue_offset));
    }

private:
    explicit shm_spsc_channel(shared_memory&& shm) : shm_(std::move(shm)) {}

    shared_memory shm_;
};

} // namespace steroids
