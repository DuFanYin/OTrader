#include "runtime/live/engine_event.hpp"
#include "utilities/event.hpp"
#include <chrono>
#include <gtest/gtest.h>
#include <thread>

namespace {

using utilities::Event;
using utilities::EventType;

TEST(LiveEventEngineBackpressure, QueueFullDropsPayloadsButEngineRemainsStoppable) {
    // No MainEngine: focus on backpressure/drop boundary + payload release + stop/drain.
    engines::EventEngine ee(nullptr /*main*/, 1);
    ee.start();

    // Burst large enough to overflow rings; put() will drop and must release payloads.
    for (int i = 0; i < 5000; ++i) {
        if (auto* ord = ee.acquire_order()) {
            ord->orderid = "OID-" + std::to_string(i);
            ee.put_event(Event(EventType::Order, ord));
        }
        if (auto* tr = ee.acquire_trade()) {
            tr->tradeid = "TID-" + std::to_string(i);
            ee.put_event(Event(EventType::Trade, tr));
        }
    }

    // Give workers time to run; then stop must always return.
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    ee.stop();

    // After stop, we should still be able to acquire/release many payloads: no pool leak.
    for (int i = 0; i < 500; ++i) {
        auto* ord = ee.acquire_order();
        ASSERT_NE(ord, nullptr);
        ee.release_order(ord);
    }
}

} // namespace

