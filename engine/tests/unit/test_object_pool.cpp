/**
 * Object pool tests.
 * Link: utilities_cpp + GTest.
 */
#include "utilities/object_pool.hpp"
#include <gtest/gtest.h>
#include <thread>
#include <unordered_set>
#include <vector>

namespace {

using namespace utilities;

struct Dummy {
    int id = 0;
    Dummy() = default;
    explicit Dummy(int i) : id(i) {}
};

// ----- basics -----
TEST(ObjectPool, AcquireReleaseReuse) {
    ObjectPool<Dummy> pool(8);
    std::vector<Dummy*> ptrs;
    const size_t N = 20;
    for (size_t i = 0; i < N; ++i) {
        Dummy* p = pool.acquire();
        ASSERT_NE(p, nullptr);
        p->id = static_cast<int>(i);
        ptrs.push_back(p);
    }
    for (Dummy* p : ptrs)
        pool.release(p);
    ptrs.clear();
    for (size_t i = 0; i < N; ++i) {
        Dummy* p = pool.acquire();
        ASSERT_NE(p, nullptr);
        ptrs.push_back(p);
    }
    for (Dummy* p : ptrs)
        pool.release(p);
}

TEST(ObjectPool, Emplace) {
    ObjectPool<Dummy> pool(8);
    Dummy* p = pool.emplace(42);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->id, 42);
    pool.release(p);
    p = pool.emplace(99);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->id, 99);
    pool.release(p);
}

TEST(ObjectPool, ReleaseNullIsNoOp) {
    ObjectPool<Dummy> pool(8);
    pool.release(nullptr);
    pool.release(nullptr);
}

// ----- An object handed out again is as new; the pool grows past its first chunk -----
TEST(ObjectPool, AnObjectHandedOutAgainIsAsNew) {
    ObjectPool<Dummy> pool(1);
    Dummy* p = pool.emplace(42);
    ASSERT_NE(p, nullptr);
    pool.release(p);
    Dummy* q = pool.acquire();
    ASSERT_EQ(q, p); // the same object
    EXPECT_EQ(q->id, 0);
    pool.release(q);
}

TEST(ObjectPool, GrowsPastItsFirstChunk) {
    ObjectPool<Dummy> pool(8);
    std::unordered_set<Dummy*> out;
    for (int i = 0; i < 1000; ++i) {
        Dummy* p = pool.emplace(i);
        ASSERT_NE(p, nullptr);
        EXPECT_TRUE(out.insert(p).second);
    }
    for (Dummy* p : out) {
        EXPECT_EQ(p->id >= 0 && p->id < 1000, true);
        EXPECT_TRUE(pool.release_checked(p));
    }
    EXPECT_GE(pool.size(), 1000U);
    Dummy stranger;
    EXPECT_FALSE(pool.release_checked(&stranger));
}

// ----- acquire after close returns nullptr -----
TEST(ObjectPool, AfterCloseAcquireReturnsNull) {
    ObjectPool<Dummy> pool(8);
    pool.close();
    EXPECT_EQ(pool.acquire(), nullptr);
    EXPECT_EQ(pool.emplace(), nullptr);
}

TEST(ObjectPool, AfterCloseExistingAcquireCanStillRelease) {
    ObjectPool<Dummy> pool(8);
    Dummy* p = pool.acquire();
    ASSERT_NE(p, nullptr);
    pool.close();
    pool.release(p);
}

// ----- double release: the second is a no-op (already out of in_use_, erase returns 0) -----
TEST(ObjectPool, DoubleReleaseSecondIsNoOp) {
    ObjectPool<Dummy> pool(8);
    Dummy* p = pool.acquire();
    ASSERT_NE(p, nullptr);
    pool.release(p);
    pool.release(p);
    pool.release(p);
}

// ----- concurrent acquire/release -----
TEST(ObjectPool, ConcurrentAcquireRelease) {
    ObjectPool<Dummy> pool(64);
    const int num_threads = 4;
    const int per_thread = 2000;
    std::vector<std::thread> threads;
    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < per_thread; ++i) {
                Dummy* p = pool.acquire();
                if (p) {
                    p->id = i;
                    pool.release(p);
                }
            }
        });
    }
    for (auto& th : threads)
        th.join();
}

} // namespace
