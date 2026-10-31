# steroids

[![ci](https://github.com/nfedotenko/steroids/actions/workflows/ci.yml/badge.svg)](https://github.com/nfedotenko/steroids/actions/workflows/ci.yml)

Low-latency C++20 building blocks: the data structures and memory-layout
tricks that hot paths are made of, extracted into a header-only library.
[steroidslog](https://github.com/nfedotenko/steroidslog) was first; the rest
of the gym is here.

## What's inside

| Primitive | Status |
| :-- | :-- |
| `steroids::spsc_queue` — bounded SPSC ring, owner-grouped cache lines | ✅ |
| `steroids::mpsc_queue` — bounded MPSC, Vyukov-style sequenced cells | ✅ |
| `steroids::id_map` — lock-free id→value interning (wait-free readers) + constexpr FNV-1a | ✅ |
| `steroids::perfect_map` — compile-time perfect hashing for fixed string keys, SIMD compare (after Lemire & Thiesen 2026) | ✅ |
| `steroids::pool_allocator` — thread-caching size-class pool, O(1) hot path | ✅ |
| `steroids::out_of_line` — hot/cold field splitting with RAII, sharded registry | ✅ |
| `steroids::spin_lock` — TTAS with pause | ✅ |
| `steroids::seqlock` — torn-free snapshots, TSan-clean by construction | ✅ |
| `steroids::shm_spsc_channel` — SPSC queue across processes in named shared memory, validated layout header | ✅ |
| C++26 reflection-based serialization codegen | planned |

Built in public, one primitive at a time. Each one lands with tests
(ASan/UBSan/TSan in CI) and — before it is called fast — a reproducible
benchmark against the strongest available alternative, reported in latency
percentiles with raw data in the repo. No benchmark, no performance claim.

## Usage

Header-only, C++20:

```cmake
add_subdirectory(steroids)            # or FetchContent
target_link_libraries(app PRIVATE steroids::steroids)
```

```cpp
#include <steroids/queues/spsc_queue.h>

steroids::spsc_queue<Order, 1024> q;
q.emplace(...);   // producer thread
Order o;
q.dequeue(o);     // consumer thread
```

## Benchmarks

```bash
cmake -B build-bench -DCMAKE_BUILD_TYPE=Release -DSTEROIDS_BUILD_BENCH=ON
cmake --build build-bench
./build-bench/bench/alloc-bench --cpu 2 --summary-csv out.csv
```

Allocate and free are timed separately with serialized tick reads, reported
as percentiles. Numbers appear in this README only once collected on a
quiet, pinned Linux box — and they will include the runs we lose.

## Tests

```bash
cmake -B build -DSTEROIDS_BUILD_TESTS=ON && cmake --build build
ctest --test-dir build
```

## License

MIT
