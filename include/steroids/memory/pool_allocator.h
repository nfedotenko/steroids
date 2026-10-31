/*
 * Copyright(c) 2026 Nikolai Fedotenko.
 * Distributed under the MIT License (http://opensource.org/licenses/MIT)
 */

#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <new>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <sys/mman.h>
#endif

namespace steroids {

//------------------------------------------------------------------------------
// Thread-caching size-class pool.
//
// pool_allocate rounds the request up to a power-of-two class between 64 B
// and 16 KiB and serves it from a per-thread free list in O(1) with no
// atomics, no locks and no syscalls; empty lists are refilled by carving a
// 256 KiB chunk from the OS. Larger requests go straight to mmap /
// VirtualAlloc.
//
// Contract (v1, by design, enforced only by documentation):
//  - deallocate on the thread that allocated: free lists are thread-local
//    and never synchronized. Cross-thread frees corrupt them.
//  - chunk memory is never returned to the OS; it is reused for the
//    lifetime of the thread. A thread's cache is released only at thread
//    exit (whole chunks; individual outstanding blocks must be freed
//    before that).
//  - blocks are aligned to min(block size, 4096); the STL adapter rejects
//    types over-aligned beyond 64 at compile time.
//------------------------------------------------------------------------------

inline constexpr std::size_t pool_min_shift = 6;  // 64 B
inline constexpr std::size_t pool_max_shift = 14; // 16 KiB
inline constexpr std::size_t pool_class_count =
    pool_max_shift - pool_min_shift + 1;
inline constexpr std::size_t pool_chunk_bytes = std::size_t{1} << 18; // 256K

namespace detail {

inline void* os_allocate(std::size_t bytes) {
#if defined(_WIN32)
    void* p = VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE,
                           PAGE_READWRITE);
    if (!p) {
        throw std::bad_alloc();
    }
    return p;
#else
    void* p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        throw std::bad_alloc();
    }
    return p;
#endif
}

inline void os_deallocate(void* ptr, std::size_t bytes) noexcept {
#if defined(_WIN32)
    (void)bytes;
    VirtualFree(ptr, 0, MEM_RELEASE);
#else
    munmap(ptr, bytes);
#endif
}

struct free_node {
    free_node* next;
};

// Size in bytes of class `idx`.
constexpr std::size_t class_block_size(std::size_t idx) noexcept {
    return std::size_t{1} << (pool_min_shift + idx);
}

// Class index for a request of `size` bytes; size must fit the largest
// class.
constexpr std::size_t class_index(std::size_t size) noexcept {
    std::size_t block = std::bit_ceil(size);
    if (block < (std::size_t{1} << pool_min_shift)) {
        block = std::size_t{1} << pool_min_shift;
    }
    return static_cast<std::size_t>(std::countr_zero(block)) - pool_min_shift;
}

class thread_pool {
public:
    thread_pool() noexcept = default;
    thread_pool(const thread_pool&) = delete;
    thread_pool& operator=(const thread_pool&) = delete;

    ~thread_pool() {
        // Chunks go back to the OS when the thread exits. Outstanding
        // blocks from this cache must have been freed by now (contract).
        for (chunk_header* c = chunks_; c;) {
            chunk_header* next = c->next;
            os_deallocate(c, pool_chunk_bytes);
            c = next;
        }
    }

    void* allocate(std::size_t idx) {
        if (free_node* n = lists_[idx]) {
            lists_[idx] = n->next;
            return n;
        }
        return refill(idx);
    }

    void deallocate(void* p, std::size_t idx) noexcept {
        auto* n = static_cast<free_node*>(p);
        n->next = lists_[idx];
        lists_[idx] = n;
    }

private:
    struct chunk_header {
        chunk_header* next;
    };

    // Carve a fresh chunk into blocks of class `idx` and hand out the
    // first one. The chunk's first block hosts the chunk header: block
    // sizes start at 64, so the header always fits.
    void* refill(std::size_t idx) {
        const std::size_t bs = class_block_size(idx);
        char* base = static_cast<char*>(os_allocate(pool_chunk_bytes));

        auto* header = reinterpret_cast<chunk_header*>(base);
        header->next = chunks_;
        chunks_ = header;

        const std::size_t count = pool_chunk_bytes / bs;
        for (std::size_t i = count; i-- > 1;) {
            auto* node = reinterpret_cast<free_node*>(base + i * bs);
            node->next = lists_[idx];
            lists_[idx] = node;
        }

        free_node* first = lists_[idx];
        lists_[idx] = first->next;
        return first;
    }

    free_node* lists_[pool_class_count]{};
    chunk_header* chunks_{nullptr};
};

inline thread_pool& tls_pool() {
    static thread_local thread_pool pool;
    return pool;
}

} // namespace detail

inline void* pool_allocate(std::size_t size) {
    if (size == 0) {
        size = 1;
    }
    if (size > (std::size_t{1} << pool_max_shift)) {
        return detail::os_allocate(size);
    }
    return detail::tls_pool().allocate(detail::class_index(size));
}

inline void pool_deallocate(void* ptr, std::size_t size) noexcept {
    if (!ptr) {
        return;
    }
    if (size == 0) {
        size = 1;
    }
    if (size > (std::size_t{1} << pool_max_shift)) {
        detail::os_deallocate(ptr, size);
        return;
    }
    detail::tls_pool().deallocate(ptr, detail::class_index(size));
}

// STL-compatible adapter. Same contract as the raw calls: containers using
// it must allocate and deallocate on one thread.
template <typename T>
struct pool_allocator {
    static_assert(alignof(T) <= 64,
                  "pool blocks are 64-byte aligned at most for small classes");

    using value_type = T;

    pool_allocator() noexcept = default;

    template <typename U>
    pool_allocator(const pool_allocator<U>&) noexcept {}

    T* allocate(std::size_t n) {
        if (n > ~std::size_t{0} / sizeof(T)) {
            throw std::bad_alloc();
        }
        return static_cast<T*>(pool_allocate(n * sizeof(T)));
    }

    void deallocate(T* p, std::size_t n) noexcept {
        pool_deallocate(p, n * sizeof(T));
    }

    friend bool operator==(const pool_allocator&,
                           const pool_allocator&) noexcept {
        return true;
    }
};

} // namespace steroids
