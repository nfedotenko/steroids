/*
 * Copyright(c) 2025 Nikolai Fedotenko.
 * Distributed under the MIT License (http://opensource.org/licenses/MIT)
 */

#include <gtest/gtest.h>

#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <steroids/memory/out_of_line.h>

namespace {

struct SessionCold {
    std::string path;
    std::string note;
};

struct Session : steroids::out_of_line<Session, SessionCold> {
    int fd{-1};
};

} // namespace

TEST(OutOfLine, FastObjectStaysHotSized) {
    // The whole point: cold members do not inflate the fast object.
    static_assert(sizeof(Session) == sizeof(int));
    SUCCEED();
}

TEST(OutOfLine, ColdRoundtrip) {
    Session s;
    s.fd = 7;
    s.cold().path = "/var/log/x";
    s.cold().note = "hello";
    EXPECT_EQ(s.cold().path, "/var/log/x");
    EXPECT_EQ(s.cold().note, "hello");
}

TEST(OutOfLine, CopyIsIndependent) {
    Session a;
    a.cold().path = "one";
    Session b(a);
    b.cold().path = "two";
    EXPECT_EQ(a.cold().path, "one");
    EXPECT_EQ(b.cold().path, "two");

    Session c;
    c = a;
    c.cold().path = "three";
    EXPECT_EQ(a.cold().path, "one");
    EXPECT_EQ(c.cold().path, "three");
}

TEST(OutOfLine, MoveCarriesColdToTheNewAddress) {
    Session a;
    a.fd = 42;
    a.cold().path = "moved";
    Session b(std::move(a));
    b.fd = 42;
    EXPECT_EQ(b.cold().path, "moved");

    Session c;
    c = std::move(b);
    EXPECT_EQ(c.cold().path, "moved");
}

TEST(OutOfLine, SurvivesVectorReallocation) {
    std::vector<Session> v;
    for (int i = 0; i < 1000; ++i) {
        Session s;
        s.fd = i;
        s.cold().path = "p" + std::to_string(i);
        v.push_back(std::move(s)); // reallocation moves every element
    }
    for (int i = 0; i < 1000; ++i) {
        EXPECT_EQ(v[static_cast<std::size_t>(i)].fd, i);
        EXPECT_EQ(v[static_cast<std::size_t>(i)].cold().path,
                  "p" + std::to_string(i));
    }
}

TEST(OutOfLine, ChurnExercisesTombstonesAndGrowth) {
    for (int round = 0; round < 50; ++round) {
        std::vector<Session> batch(200);
        for (int i = 0; i < 200; ++i) {
            batch[static_cast<std::size_t>(i)].cold().note =
                std::to_string(round * 1000 + i);
        }
        for (int i = 0; i < 200; ++i) {
            ASSERT_EQ(batch[static_cast<std::size_t>(i)].cold().note,
                      std::to_string(round * 1000 + i));
        }
    }
}

// Objects on different threads create, move, read and destroy concurrently;
// shards synchronize the registry. Run under TSan in CI.
TEST(OutOfLine, ParallelLifecycles) {
    auto hammer = [](int seed) {
        for (int round = 0; round < 100; ++round) {
            std::vector<Session> v;
            for (int i = 0; i < 50; ++i) {
                Session s;
                s.cold().note = std::to_string(seed + i);
                v.push_back(std::move(s));
            }
            for (int i = 0; i < 50; ++i) {
                ASSERT_EQ(v[static_cast<std::size_t>(i)].cold().note,
                          std::to_string(seed + i));
            }
        }
    };
    std::thread t1(hammer, 10'000);
    std::thread t2(hammer, 20'000);
    std::thread t3(hammer, 30'000);
    t1.join();
    t2.join();
    t3.join();
}
