#include "runtime/live/engine_event.hpp"
#include "utilities/event.hpp"
#include <gtest/gtest.h>

namespace {

TEST(LiveEventEngineSafety, PutBeforeStartDoesNotCrash) {
    engines::EventEngine ee(nullptr /*main*/, 1);

    // Not started => should reject safely (no crash).
    auto* snap = ee.acquire_snapshot();
    ASSERT_NE(snap, nullptr);
    snap->portfolio_name = "SPXW";
    ee.put_event(utilities::Event(utilities::EventType::Snapshot, snap));

    auto* ord = ee.acquire_order();
    ASSERT_NE(ord, nullptr);
    ord->orderid = "OID";
    ee.put_event(utilities::Event(utilities::EventType::Order, ord));

    auto* tr = ee.acquire_trade();
    ASSERT_NE(tr, nullptr);
    tr->tradeid = "TID";
    ee.put_event(utilities::Event(utilities::EventType::Trade, tr));
}

TEST(LiveEventEngineSafety, StopReturns) {
    engines::EventEngine ee(nullptr /*main*/, 1);
    ee.start();

    // Enqueue a burst of payload events; even if some get dropped, payloads must be released.
    for (int i = 0; i < 200; ++i) {
        auto* ord = ee.acquire_order();
        ASSERT_NE(ord, nullptr);
        ord->orderid = "OID-" + std::to_string(i);
        ee.put_event(utilities::Event(utilities::EventType::Order, ord));
    }
    for (int i = 0; i < 200; ++i) {
        auto* tr = ee.acquire_trade();
        ASSERT_NE(tr, nullptr);
        tr->tradeid = "TID-" + std::to_string(i);
        ee.put_event(utilities::Event(utilities::EventType::Trade, tr));
    }

    ee.stop();
}

} // namespace
