/**
 * SpscRing tests — §8.2
 * Link: utilities_cpp + GTest. No core/runtime.
 */
#include "utilities/spsc_ring.hpp"
#include <gtest/gtest.h>
#include <thread>
#include <vector>

namespace {

using namespace utilities;

constexpr size_t kCap = 256;

TEST(SpscRing, OrderNoLoss) {
    SpscRing<int, kCap> ring;
    const int N = 1000;
    std::thread prod([&] {
        for (int i = 0; i < N; ++i) {
            while (!ring.try_push(i))
                std::this_thread::yield();
        }
    });
    std::vector<int> out;
    out.reserve(N);
    std::thread cons([&] {
        for (int i = 0; i < N; ++i) {
            int v = -1;
            while (!ring.try_pop(v))
                std::this_thread::yield();
            out.push_back(v);
        }
    });
    prod.join();
    cons.join();
    ASSERT_EQ(out.size(), static_cast<size_t>(N));
    for (int i = 0; i < N; ++i)
        EXPECT_EQ(out[i], i);
}

TEST(SpscRing, FullReturnsFalse) {
    SpscRing<int, 8> r;
    for (size_t i = 0; i < 8; ++i)
        ASSERT_TRUE(r.try_push(static_cast<int>(i)));
    EXPECT_FALSE(r.try_push(99));
    int v = -1;
    ASSERT_TRUE(r.try_pop(v));
    EXPECT_EQ(v, 0);
    EXPECT_TRUE(r.try_push(99));
}

TEST(SpscRing, EmptyReturnsFalse) {
    SpscRing<int, 8> r;
    int v = -1;
    EXPECT_FALSE(r.try_pop(v));
    EXPECT_TRUE(r.empty());
}

TEST(SpscRing, CapacityAndSizeApprox) {
    SpscRing<int, 16> r;
    EXPECT_EQ(r.capacity(), 16u);
    EXPECT_TRUE(r.empty());
    r.try_push(1);
    r.try_push(2);
    EXPECT_EQ(r.size_approx(), 2u);
    int v = 0;
    r.try_pop(v);
    EXPECT_EQ(r.size_approx(), 1u);
}

} // namespace
