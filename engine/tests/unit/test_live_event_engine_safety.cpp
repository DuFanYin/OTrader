#include "core/engine_event.hpp"
#include "model/event.hpp"
#include <gtest/gtest.h>

namespace {

TEST(LiveEventEngineSafety, PutBeforeStartDoesNotCrash) {
    core::EventEngine ee(nullptr /*main*/);

    // Not started => should reject safely (no crash).
    auto* snap = ee.acquire_snapshot();
    ASSERT_NE(snap, nullptr);
    snap->portfolio_name = "SPXW";
    ee.put_event(model::Event(model::EventType::Snapshot, snap));

    auto* ord = ee.acquire_order();
    ASSERT_NE(ord, nullptr);
    ord->orderid = "OID";
    ee.put_event(model::Event(model::EventType::Order, ord));

    auto* tr = ee.acquire_trade();
    ASSERT_NE(tr, nullptr);
    tr->tradeid = "TID";
    ee.put_event(model::Event(model::EventType::Trade, tr));
}

TEST(LiveEventEngineSafety, StopReturns) {
    core::EventEngine ee(nullptr /*main*/);
    ee.start();

    // Enqueue a burst of payload events; even if some get dropped, payloads must be released.
    for (int i = 0; i < 200; ++i) {
        auto* ord = ee.acquire_order();
        ASSERT_NE(ord, nullptr);
        ord->orderid = "OID-" + std::to_string(i);
        ee.put_event(model::Event(model::EventType::Order, ord));
    }
    for (int i = 0; i < 200; ++i) {
        auto* tr = ee.acquire_trade();
        ASSERT_NE(tr, nullptr);
        tr->tradeid = "TID-" + std::to_string(i);
        ee.put_event(model::Event(model::EventType::Trade, tr));
    }

    ee.stop();
}

} // namespace
