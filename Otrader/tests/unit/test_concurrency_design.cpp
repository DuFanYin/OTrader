#include "utilities/mpsc_ring.hpp"
#include "utilities/object_pool.hpp"
#include <atomic>
#include <chrono>
#include <gtest/gtest.h>
#include <thread>
#include <unordered_set>
#include <vector>

namespace {

struct Msg {
    int producer_id = -1;
    int seq = -1;
};

TEST(ConcurrencyDesign, MpscQueueNoLossWithObjectPool) {
    constexpr size_t kCap = 4096;
    constexpr int kProducers = 4;
    constexpr int kPerProducer = 800;
    constexpr int kTotal = kProducers * kPerProducer;

    utilities::ObjectPool<Msg> pool(2048);
    utilities::MpscRing<Msg*, kCap> ring;

    std::vector<std::thread> producers;
    producers.reserve(kProducers);

    for (int pid = 0; pid < kProducers; ++pid) {
        producers.emplace_back([&, pid] {
            for (int i = 0; i < kPerProducer; ++i) {
                Msg* m = nullptr;
                while ((m = pool.acquire()) == nullptr) {
                    std::this_thread::yield();
                }
                m->producer_id = pid;
                m->seq = i;
                while (!ring.try_push(m)) {
                    std::this_thread::yield();
                }
            }
        });
    }

    std::unordered_set<long long> received;
    received.reserve(static_cast<size_t>(kTotal));
    std::thread consumer([&] {
        for (int i = 0; i < kTotal; ++i) {
            Msg* m = nullptr;
            while (!ring.try_pop(m)) {
                std::this_thread::yield();
            }
            ASSERT_NE(m, nullptr);
            const long long key =
                static_cast<long long>(m->producer_id) * 1000000LL + static_cast<long long>(m->seq);
            received.insert(key);
            pool.release(m);
        }
    });

    for (auto& t : producers) {
        t.join();
    }
    consumer.join();

    EXPECT_EQ(received.size(), static_cast<size_t>(kTotal));
}

TEST(ConcurrencyDesign, ObjectPoolCloseStopsNewAcquireAcrossThreads) {
    utilities::ObjectPool<Msg> pool(64);
    std::atomic<bool> run{true};
    std::atomic<int> acquired{0};

    std::thread worker([&] {
        while (run.load(std::memory_order_relaxed)) {
            Msg* m = pool.acquire();
            if (m == nullptr) {
                break;
            }
            ++acquired;
            pool.release(m);
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    pool.close();
    run.store(false, std::memory_order_relaxed);
    worker.join();

    EXPECT_EQ(pool.acquire(), nullptr);
    EXPECT_GE(acquired.load(), 1);
}

} // namespace
