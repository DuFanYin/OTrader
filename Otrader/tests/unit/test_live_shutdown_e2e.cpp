#include "runtime/live/engine_event.hpp"
#include "utilities/event.hpp"
#include <atomic>
#include <chrono>
#include <gtest/gtest.h>
#include <thread>
#include <vector>

namespace {

TEST(LiveShutdownE2E, StopAfterQueuedPayloadsDrainsNoLeak) {
    engines::EventEngine ee(nullptr /*main*/, 1);
    ee.start();

    for (int i = 0; i < 400; ++i) {
        if (auto* ord = ee.acquire_order()) {
            ord->orderid = "OID-" + std::to_string(i);
            ee.put_event(utilities::Event(utilities::EventType::Order, ord));
        }
        if (auto* tr = ee.acquire_trade()) {
            tr->tradeid = "TID-" + std::to_string(i);
            ee.put_event(utilities::Event(utilities::EventType::Trade, tr));
        }
    }

    ee.stop();
}

TEST(LiveShutdownE2E, StopWhileProducersRunningReturnsAndRejects) {
    engines::EventEngine ee(nullptr /*main*/, 1);
    ee.start();

    std::atomic<bool> run{true};
    constexpr int kProducers = 4;
    std::vector<std::thread> producers;
    producers.reserve(kProducers);

    for (int t = 0; t < kProducers; ++t) {
        producers.emplace_back([&] {
            int i = 0;
            while (run.load(std::memory_order_relaxed)) {
                if (auto* ord = ee.acquire_order()) {
                    ord->orderid = "OID-" + std::to_string(i++);
                    ee.put_event(utilities::Event(utilities::EventType::Order, ord));
                }
                if (auto* tr = ee.acquire_trade()) {
                    tr->tradeid = "TID-" + std::to_string(i++);
                    ee.put_event(utilities::Event(utilities::EventType::Trade, tr));
                }
            }
        });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    ee.stop();
    run.store(false, std::memory_order_relaxed);
    for (auto& th : producers) {
        th.join();
    }
}

} // namespace
