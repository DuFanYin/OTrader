/** MpscQueue: an unbounded queue of pooled nodes, many producers, one consumer. */
#include "utilities/mpsc_queue.hpp"
#include "utilities/object_pool.hpp"
#include <atomic>
#include <gtest/gtest.h>
#include <thread>
#include <vector>

namespace {

/** A node of the queue, from a pool as the event loop's are. */
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

TEST(MpscQueue, KeepsEachProducersOrderAndLosesNothing) {
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

} // namespace
