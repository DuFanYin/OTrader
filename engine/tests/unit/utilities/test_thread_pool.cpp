/** ThreadPool: parallel_for runs each index once, called from many threads at once. */
#include "utilities/thread_pool.hpp"
#include <atomic>
#include <cstddef>
#include <gtest/gtest.h>
#include <memory>
#include <thread>
#include <vector>

namespace {

TEST(ThreadPool, RunsEachIndexOnceFromManyCallers) {
    utilities::ThreadPool pool(4);
    constexpr int kCallers = 4;
    constexpr std::size_t kRange = 5000;
    std::vector<std::thread> callers;
    std::atomic<int> wrong{0};
    for (int c = 0; c < kCallers; ++c) {
        callers.emplace_back([&] {
            auto hits = std::make_unique<std::atomic<int>[]>(kRange);
            for (int round = 0; round < 100; ++round) { // busy with another's: inline
                for (std::size_t i = 0; i < kRange; ++i) {
                    hits[i].store(0, std::memory_order_relaxed);
                }
                pool.parallel_for(0, kRange, [&](std::size_t i) {
                    hits[i].fetch_add(1, std::memory_order_relaxed);
                });
                for (std::size_t i = 0; i < kRange; ++i) {
                    if (hits[i].load(std::memory_order_relaxed) != 1) {
                        ++wrong;
                    }
                }
            }
        });
    }
    for (auto& t : callers) {
        t.join();
    }
    EXPECT_EQ(wrong.load(), 0);
}

} // namespace
