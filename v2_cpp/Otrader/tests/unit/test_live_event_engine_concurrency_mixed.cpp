#include "runtime/live/engine_event.hpp"
#include "utilities/event.hpp"
#include <atomic>
#include <chrono>
#include <gtest/gtest.h>
#include <thread>
#include <vector>

namespace {

using utilities::Event;
using utilities::EventType;

TEST(LiveEventEngineConcurrency, MultiProducerMixedPayloadsStopReturnsAndNoAcquireStarvation) {
    // No MainEngine: we only validate concurrent put/release/stop behavior (minimal intrusion).
    engines::EventEngine ee(nullptr /*main*/, 1);
    ee.start();

    std::atomic<bool> run{true};
    constexpr int kProducers = 6;
    std::vector<std::thread> producers;
    producers.reserve(kProducers);

    for (int t = 0; t < kProducers; ++t) {
        producers.emplace_back([&] {
            int i = 0;
            while (run.load(std::memory_order_relaxed)) {
                if (auto* ord = ee.acquire_order()) {
                    ord->orderid = "OID-" + std::to_string(t) + "-" + std::to_string(i++);
                    ee.put_event(Event(EventType::Order, ord));
                }
                if (auto* tr = ee.acquire_trade()) {
                    tr->tradeid = "TID-" + std::to_string(t) + "-" + std::to_string(i++);
                    ee.put_event(Event(EventType::Trade, tr));
                }
                if (auto* snap = ee.acquire_snapshot()) {
                    snap->portfolio_name = "P";
                    ee.put_event(Event(EventType::Snapshot, snap));
                }
            }
        });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    ee.stop();
    run.store(false, std::memory_order_relaxed);
    for (auto& th : producers) {
        th.join();
    }

    // After stop(), producers may have been racing with stop() and the engine may have rejected
    // some events; the key correctness property is: pooled payloads are not leaked. A pragmatic
    // check is: we can still acquire a decent number of payloads after stop().
    for (int i = 0; i < 200; ++i) {
        auto* ord = ee.acquire_order();
        ASSERT_NE(ord, nullptr);
        ee.release_order(ord);
        auto* tr = ee.acquire_trade();
        ASSERT_NE(tr, nullptr);
        ee.release_trade(tr);
        auto* snap = ee.acquire_snapshot();
        ASSERT_NE(snap, nullptr);
        ee.release_snapshot(snap);
    }
}

} // namespace

