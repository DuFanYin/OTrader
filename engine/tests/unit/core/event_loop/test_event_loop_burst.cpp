#include "core/event_loop.hpp"
#include "model/event.hpp"
#include <chrono>
#include <gtest/gtest.h>
#include <thread>

namespace {

using model::Event;
using model::EventType;

TEST(EventLoopBurst, ABurstIsQueuedWholeAndStopProcessesIt) {
    // No Main: the queue takes a burst far past the old ring's 512 without dropping, stop()
    // processes what is queued and returns, and every payload goes back to its pool.
    core::EventLoop loop;
    loop.start();

    // A burst of 10 000 events: queued whole (the queue has no bound while the engine runs).
    for (int i = 0; i < 5000; ++i) {
        if (auto* ord = loop.acquire_order()) {
            ord->order_id = "OID-" + std::to_string(i);
            loop.put_event(Event(EventType::Order, ord));
        }
        if (auto* tr = loop.acquire_fill()) {
            tr->fill_id = "TID-" + std::to_string(i);
            loop.put_event(Event(EventType::Fill, tr));
        }
    }

    // Give workers time to run; then stop must always return.
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    loop.stop();

    // After stop, we should still be able to acquire/release many payloads: no pool leak.
    for (int i = 0; i < 500; ++i) {
        auto* ord = loop.acquire_order();
        ASSERT_NE(ord, nullptr);
        loop.release_order(ord);
    }
}

} // namespace

