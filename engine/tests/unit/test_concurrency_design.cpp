#include "utilities/mpsc_queue.hpp"
#include "utilities/mpsc_ring.hpp"
#include "utilities/object_pool.hpp"
#include "utilities/thread_pool.hpp"
#include "utilities/wakeup.hpp"
#include <atomic>
#include <chrono>
#include <gtest/gtest.h>
#include <memory>
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

/** A node of the unbounded queue, from a pool as the event engine's are. */
struct Node {
    int producer_id = -1;
    int seq = -1;
    std::atomic<Node*> next{nullptr};

    Node() = default;
    Node(const Node&) = delete;
    Node& operator=(Node&& other) noexcept {
        producer_id = other.producer_id;
        seq = other.seq;
        return *this;
    }
};

TEST(ConcurrencyDesign, MpscQueueKeepsEachProducersOrderAndLosesNothing) {
    constexpr int kProducers = 4;
    constexpr int kEach = 50'000;
    utilities::ObjectPool<Node> pool(64);
    utilities::MpscQueue<Node> queue;
    std::vector<std::thread> producers;
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&, p] {
            for (int i = 0; i < kEach; ++i) {
                Node* n = pool.acquire();
                ASSERT_NE(n, nullptr);
                n->producer_id = p;
                n->seq = i;
                queue.push(n);
            }
        });
    }
    std::vector<int> next(kProducers, 0);
    int received = 0;
    while (received < kProducers * kEach) {
        Node* n = queue.pop();
        if (n == nullptr) {
            std::this_thread::yield();
            continue;
        }
        EXPECT_EQ(n->seq, next[n->producer_id]++) << "producer " << n->producer_id;
        ++received;
        pool.release(n);
    }
    for (auto& t : producers) {
        t.join();
    }
    EXPECT_EQ(queue.pop(), nullptr);
    EXPECT_TRUE(queue.empty());
}

TEST(ConcurrencyDesign, WakeupKeepsItsDeadlineAndWakesTheSleeper) {
    using Clock = utilities::Wakeup::Clock;
    utilities::Wakeup wakeup;
    const auto t0 = Clock::now();
    wakeup.wait_until(t0 + std::chrono::milliseconds(30), [] { return false; });
    EXPECT_GE(Clock::now() - t0, std::chrono::milliseconds(30));

    for (int round = 0; round < 200; ++round) { // the wake between its check and its sleep too
        std::atomic<bool> work{false};
        std::thread waker([&] {
            work.store(true, std::memory_order_release);
            wakeup.wake();
        });
        const auto deadline = Clock::now() + std::chrono::seconds(10);
        while (!work.load(std::memory_order_acquire) && Clock::now() < deadline) {
            wakeup.wait_until(deadline, [&] { return work.load(std::memory_order_acquire); });
        }
        waker.join();
        ASSERT_LT(Clock::now(), deadline) << "round " << round << ": a wake was lost";
    }
}

TEST(ConcurrencyDesign, ThreadPoolRunsEachIndexOnceFromManyCallers) {
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
