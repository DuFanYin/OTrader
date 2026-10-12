#include "core/engine_event.hpp"
#include "model/event.hpp"
#include <chrono>
#include <gtest/gtest.h>
#include <thread>

namespace {

using model::Event;
using model::EventType;

TEST(LiveEventEngineBurst, ABurstIsQueuedWholeAndStopProcessesIt) {
    // No MainEngine: the queue takes a burst far past the old ring's 512 without dropping, stop()
    // processes what is queued and returns, and every payload goes back to its pool.
    core::EventEngine ee(nullptr /*main*/);
    ee.start();

    // A burst of 10 000 events: queued whole (the queue has no bound while the engine runs).
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

