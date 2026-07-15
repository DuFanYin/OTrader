/**
 * Object pool tests — §8.3
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

// ----- 基础 -----
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

// ----- close 后 acquire 返回 nullptr -----
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

// ----- double-release：第二次 no-op（in_use_ 已删，erase 返回 0） -----
TEST(ObjectPool, DoubleReleaseSecondIsNoOp) {
    ObjectPool<Dummy> pool(8);
    Dummy* p = pool.acquire();
    ASSERT_NE(p, nullptr);
    pool.release(p);
    pool.release(p);
    pool.release(p);
}

// ----- 并发 acquire/release -----
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
