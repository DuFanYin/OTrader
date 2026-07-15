#include "utilities/mpsc_ring.hpp"
#include "utilities/object_pool.hpp"
#include <atomic>
#include <chrono>
#include <gtest/gtest.h>
#include <thread>

namespace {

struct Msg {
    int id = -1;
};

TEST(ConcurrencyShutdown, ProducerStopAndDrainWithoutLeak) {
    constexpr size_t kCap = 1024;
    utilities::ObjectPool<Msg> pool(256);
    utilities::MpscRing<Msg*, kCap> ring;

    std::atomic<bool> run{true};
    std::atomic<int> produced{0};
    std::atomic<int> consumed{0};

    std::thread producer([&] {
        int seq = 0;
        while (run.load(std::memory_order_relaxed)) {
            Msg* p = pool.acquire();
            if (p == nullptr) {
                break;
            }
            p->id = seq++;
            if (ring.try_push(p)) {
                ++produced;
            } else {
                pool.release(p);
                std::this_thread::yield();
            }
        }
    });

    std::thread consumer([&] {
        while (run.load(std::memory_order_relaxed) || !ring.empty()) {
            Msg* p = nullptr;
            if (ring.try_pop(p) && p != nullptr) {
                ++consumed;
                pool.release(p);
            } else {
                std::this_thread::yield();
            }
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    run.store(false, std::memory_order_relaxed);
    producer.join();
    consumer.join();

    // final drain (should usually be empty, but keep test deterministic)
    Msg* p = nullptr;
    while (ring.try_pop(p)) {
        if (p != nullptr) {
            ++consumed;
            pool.release(p);
        }
    }

    EXPECT_TRUE(ring.empty());
    EXPECT_EQ(produced.load(), consumed.load());
}

TEST(ConcurrencyShutdown, PoolCloseRejectsNewAcquireAfterStop) {
    utilities::ObjectPool<Msg> pool(32);
    Msg* p = pool.acquire();
    ASSERT_NE(p, nullptr);
    pool.release(p);

    pool.close();
    EXPECT_EQ(pool.acquire(), nullptr);
    EXPECT_EQ(pool.emplace(), nullptr);
}

} // namespace
