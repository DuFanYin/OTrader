/**
 * MpscRing tests.
 * Link: utilities_cpp + GTest. No core.
 */
#include "utilities/mpsc_ring.hpp"
#include <gtest/gtest.h>
#include <set>
#include <thread>
#include <vector>
#include "model/event.hpp"
#include "model/object.hpp"
#include "utilities/object_pool.hpp"
#include <atomic>
#include <chrono>
#include <cstddef>
#include <utility>
#include <variant>

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

/** Events carry pooled orders through the ring by pointer: what is popped is what was pushed. */
TEST(MpscRing, AnEventCarriesItsPooledOrderThrough) {
    utilities::ObjectPool<model::Event> event_pool;
    utilities::ObjectPool<model::Order> order_pool;
    utilities::MpscRing<model::Event*, 8> ring;
    auto* order = order_pool.acquire();
    ASSERT_NE(order, nullptr);
    order->order_id = "OID-1";
    order->symbol = "SPXW-TEST";
    auto* event = event_pool.acquire();
    ASSERT_NE(event, nullptr);
    event->type = model::EventType::Order;
    event->data = order;
    ASSERT_TRUE(ring.try_push(event));
    model::Event* popped = nullptr;
    ASSERT_TRUE(ring.try_pop(popped));
    ASSERT_EQ(popped, event);
    const auto* slot = std::get_if<model::Order*>(&popped->data);
    ASSERT_NE(slot, nullptr);
    EXPECT_EQ((*slot)->order_id, "OID-1");
    order_pool.release(*slot);
    event_pool.release(popped);
}

struct Msg {
    int producer_id = -1;
    int seq = -1;
};

TEST(MpscRing, ManyProducersWithPooledMessagesLoseNothing) {
    constexpr int kProducers = 4;
    constexpr int kPerProducer = 800;
    constexpr int kTotal = kProducers * kPerProducer;
    utilities::ObjectPool<Msg> pool(2048);
    utilities::MpscRing<Msg*, 4096> ring;
    std::vector<std::thread> producers;
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
    std::set<std::pair<int, int>> received;
    std::thread consumer([&] {
        for (int i = 0; i < kTotal; ++i) {
            Msg* m = nullptr;
            while (!ring.try_pop(m)) {
                std::this_thread::yield();
            }
            received.emplace(m->producer_id, m->seq);
            pool.release(m);
        }
    });
    for (auto& t : producers) {
        t.join();
    }
    consumer.join();
    EXPECT_EQ(received.size(), static_cast<std::size_t>(kTotal));
}

TEST(MpscRing, StoppedProducersLeaveNothingUndrained) {
    utilities::ObjectPool<Msg> pool(256);
    utilities::MpscRing<Msg*, 1024> ring;
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
            p->seq = seq++;
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
    Msg* p = nullptr;
    while (ring.try_pop(p)) {
        ++consumed;
        pool.release(p);
    }
    EXPECT_TRUE(ring.empty());
    EXPECT_EQ(produced.load(), consumed.load());
}

} // namespace
