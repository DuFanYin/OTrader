#include "core/event_loop.hpp"
#include "model/event.hpp"
#include <atomic>
#include <chrono>
#include <gtest/gtest.h>
#include <thread>
#include <vector>

namespace {

using model::Event;
using model::EventType;

TEST(EventLoopConcurrency, MultiProducerMixedPayloadsStopReturnsAndNoAcquireStarvation) {
    // No Main: we only validate concurrent put/release/stop behavior (minimal intrusion).
    core::EventLoop loop;
    loop.start();

    std::atomic<bool> run{true};
    constexpr int kProducers = 6;
    std::vector<std::thread> producers;
    producers.reserve(kProducers);

    for (int t = 0; t < kProducers; ++t) {
        producers.emplace_back([&, t] { // t by value: the loop variable dies before the thread
            int i = 0;
            while (run.load(std::memory_order_relaxed)) {
                if (auto* ord = loop.acquire_order()) {
                    ord->order_id = "OID-" + std::to_string(t) + "-" + std::to_string(i++);
                    loop.put_event(Event(EventType::Order, ord));
                }
                if (auto* tr = loop.acquire_fill()) {
                    tr->fill_id = "TID-" + std::to_string(t) + "-" + std::to_string(i++);
                    loop.put_event(Event(EventType::Fill, tr));
                }
                if (auto* snap = loop.acquire_snapshot()) {
                    snap->portfolio_name = "P";
                    loop.put_event(Event(EventType::Snapshot, snap));
                }
            }
        });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    loop.stop();
    run.store(false, std::memory_order_relaxed);
    for (auto& th : producers) {
        th.join();
    }

    // After stop(), producers may have been racing with stop() and the engine may have rejected
    // some events; the key correctness property is: pooled payloads are not leaked. A pragmatic
    // check is: we can still acquire a decent number of payloads after stop().
    for (int i = 0; i < 200; ++i) {
        auto* ord = loop.acquire_order();
        ASSERT_NE(ord, nullptr);
        loop.release_order(ord);
        auto* tr = loop.acquire_fill();
        ASSERT_NE(tr, nullptr);
        loop.release_trade(tr);
        auto* snap = loop.acquire_snapshot();
        ASSERT_NE(snap, nullptr);
        loop.release_snapshot(snap);
    }
}

} // namespace
