/*
 * Copyright(c) 2026 Nikolai Fedotenko.
 * Distributed under the MIT License (http://opensource.org/licenses/MIT)
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#if !defined(_WIN32)
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <steroids/ipc/shm_spsc_channel.h>

using steroids::shm_spsc_channel;

namespace {

struct Tick {
    std::uint64_t seq;
    std::uint64_t px;
    std::uint64_t qty;
};

std::string unique_name(const char* base) {
    return std::string(base) + "-" +
#if defined(_WIN32)
           std::to_string(GetCurrentProcessId());
#else
           std::to_string(::getpid());
#endif
}

} // namespace

// Two mappings of one segment inside one process: writes through one view
// must be visible through the other — the cross-address-space property in
// miniature.
TEST(ShmSpscChannel, TwoViewsOfOneSegment) {
    const std::string name = unique_name("steroids-test-views");
    using chan_t = shm_spsc_channel<Tick, 64>;

    auto producer_view = chan_t::create(name);
    auto consumer_view = chan_t::open(name);

    for (std::uint64_t i = 0; i < 10; ++i) {
        ASSERT_TRUE(producer_view.queue().enqueue({i, 100 + i, 7}));
    }
    Tick t{};
    for (std::uint64_t i = 0; i < 10; ++i) {
        ASSERT_TRUE(consumer_view.queue().dequeue(t));
        EXPECT_EQ(t.seq, i);
        EXPECT_EQ(t.px, 100 + i);
    }
    EXPECT_FALSE(consumer_view.queue().dequeue(t));

    chan_t::unlink(name);
}

TEST(ShmSpscChannel, LayoutMismatchIsRejected) {
    const std::string name = unique_name("steroids-test-layout");
    auto made = shm_spsc_channel<Tick, 64>::create(name);

    // Same name, different element type → loud failure, not corruption.
    using wrong_t = shm_spsc_channel<std::uint64_t, 64>;
    EXPECT_THROW((void)wrong_t::open(name), std::runtime_error);

    // Different capacity too.
    using wrong_cap_t = shm_spsc_channel<Tick, 128>;
    EXPECT_THROW((void)wrong_cap_t::open(name), std::runtime_error);

    shm_spsc_channel<Tick, 64>::unlink(name);
}

TEST(ShmSpscChannel, OpenWithoutCreateFails) {
    EXPECT_THROW(
        (void)(shm_spsc_channel<Tick, 64>::open(
            unique_name("steroids-test-nonexistent"))),
        std::runtime_error);
}

#if !defined(_WIN32) && !defined(__has_feature)
#define STEROIDS_RUN_FORK_TEST 1
#elif !defined(_WIN32) && defined(__has_feature)
#if !__has_feature(thread_sanitizer)
#define STEROIDS_RUN_FORK_TEST 1
#endif
#endif

#if defined(STEROIDS_RUN_FORK_TEST)
// The real thing: a child process produces across the boundary, the parent
// consumes. Skipped under TSan (fork + instrumented runtime do not mix).
TEST(ShmSpscChannel, ForkedProducerToParentConsumer) {
    const std::string name = unique_name("steroids-test-fork");
    using chan_t = shm_spsc_channel<Tick, 256>;
    constexpr std::uint64_t N = 100'000;

    auto parent_view = chan_t::create(name);

    const pid_t pid = ::fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) { // child: producer
        try {
            auto child_view = chan_t::open(name);
            for (std::uint64_t i = 0; i < N;) {
                if (child_view.queue().enqueue({i, i * 2, i * 3})) {
                    ++i;
                }
            }
            _exit(0);
        } catch (...) {
            _exit(1);
        }
    }

    Tick t{};
    std::uint64_t expect = 0;
    while (expect < N) {
        if (parent_view.queue().dequeue(t)) {
            ASSERT_EQ(t.seq, expect);
            ASSERT_EQ(t.px, expect * 2);
            ASSERT_EQ(t.qty, expect * 3);
            ++expect;
        }
    }

    int status = 0;
    ASSERT_EQ(::waitpid(pid, &status, 0), pid);
    EXPECT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0);

    chan_t::unlink(name);
}
#endif
