#include "utilities/event.hpp"
#include "utilities/mpsc_ring.hpp"
#include "utilities/object.hpp"
#include "utilities/object_pool.hpp"
#include <gtest/gtest.h>

namespace {

TEST(SystemDataFlow, StrategyUpdateRingRoundTrip) {
    constexpr size_t kCap = 8;
    utilities::ObjectPool<utilities::StrategyUpdateData> pool;
    utilities::MpscRing<utilities::StrategyUpdateData*, kCap> ring;

    auto* produced = pool.acquire();
    ASSERT_NE(produced, nullptr);
    produced->strategy_name = "straddle_spxw";
    produced->class_name = "straddle";
    produced->portfolio = "SPXW";
    produced->json_payload = R"({"status":"running"})";

    ASSERT_TRUE(ring.try_push(produced));

    utilities::StrategyUpdateData* consumed = nullptr;
    ASSERT_TRUE(ring.try_pop(consumed));
    ASSERT_NE(consumed, nullptr);
    EXPECT_EQ(consumed->strategy_name, "straddle_spxw");
    EXPECT_EQ(consumed->class_name, "straddle");
    EXPECT_EQ(consumed->portfolio, "SPXW");
    EXPECT_EQ(consumed->json_payload, R"({"status":"running"})");
    pool.release(consumed);
}

TEST(SystemDataFlow, EventOrderPointerFlow) {
    constexpr size_t kCap = 8;
    utilities::ObjectPool<utilities::Event> event_pool;
    utilities::ObjectPool<utilities::OrderData> order_pool;
    utilities::MpscRing<utilities::Event*, kCap> ring;

    auto* order = order_pool.acquire();
    ASSERT_NE(order, nullptr);
    order->orderid = "OID-1";
    order->symbol = "SPXW-TEST";
    order->status = utilities::Status::SUBMITTING;

    auto* event = event_pool.acquire();
    ASSERT_NE(event, nullptr);
    event->type = utilities::EventType::Order;
    event->data = order;

    ASSERT_TRUE(ring.try_push(event));

    utilities::Event* popped = nullptr;
    ASSERT_TRUE(ring.try_pop(popped));
    ASSERT_NE(popped, nullptr);
    EXPECT_EQ(popped->type, utilities::EventType::Order);

    const auto* slot = std::get_if<utilities::OrderData*>(&popped->data);
    ASSERT_NE(slot, nullptr);
    ASSERT_NE(*slot, nullptr);
    EXPECT_EQ((*slot)->orderid, "OID-1");
    EXPECT_EQ((*slot)->symbol, "SPXW-TEST");

    order_pool.release(*slot);
    event_pool.release(popped);
}

} // namespace
