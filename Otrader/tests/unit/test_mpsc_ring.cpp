/**
 * MpscRing tests — §8.2
 * Link: utilities_cpp + GTest. No core/runtime.
 */
#include "utilities/mpsc_ring.hpp"
#include <gtest/gtest.h>
#include <set>
#include <thread>
#include <vector>

namespace {

using namespace utilities;

TEST(MpscRing, MultiProducerSingleConsumer) {
    MpscRing<int, 4096> ring;
    const int P = 4;
    const int M = 500;
    std::vector<std::thread> prods;
    for (int t = 0; t < P; ++t) {
        prods.emplace_back([&, t] {
            for (int j = 0; j < M; ++j)
                while (!ring.try_push(t * M + j))
                    std::this_thread::yield();
        });
    }
    std::set<int> collected;
    std::thread cons([&] {
        for (int i = 0; i < P * M; ++i) {
            int v = -1;
            while (!ring.try_pop(v))
                std::this_thread::yield();
            collected.insert(v);
        }
    });
    for (auto& t : prods)
        t.join();
    cons.join();
    EXPECT_EQ(collected.size(), static_cast<size_t>(P * M));
    for (int i = 0; i < P * M; ++i)
        EXPECT_TRUE(collected.count(i) == 1);
}

TEST(MpscRing, FullAndEmpty) {
    MpscRing<int, 8> r;
    for (size_t i = 0; i < 8; ++i)
        ASSERT_TRUE(r.try_push(static_cast<int>(i)));
    EXPECT_FALSE(r.try_push(99));
    int v = -1;
    for (int i = 0; i < 8; ++i)
        ASSERT_TRUE(r.try_pop(v));
    EXPECT_FALSE(r.try_pop(v));
    EXPECT_TRUE(r.empty());
}

} // namespace
