#include "model/event.hpp"
#include "utilities/mpsc_ring.hpp"
#include "model/object.hpp"
#include "utilities/object_pool.hpp"
#include <gtest/gtest.h>

namespace {

TEST(SystemDataFlow, EventOrderPointerFlow) {
    constexpr size_t kCap = 8;
    utilities::ObjectPool<model::Event> event_pool;
    utilities::ObjectPool<model::OrderData> order_pool;
    utilities::MpscRing<model::Event*, kCap> ring;

    auto* order = order_pool.acquire();
    ASSERT_NE(order, nullptr);
    order->orderid = "OID-1";
    order->symbol = "SPXW-TEST";
    order->status = model::Status::SUBMITTING;

    auto* event = event_pool.acquire();
    ASSERT_NE(event, nullptr);
    event->type = model::EventType::Order;
    event->data = order;

    ASSERT_TRUE(ring.try_push(event));

    model::Event* popped = nullptr;
    ASSERT_TRUE(ring.try_pop(popped));
    ASSERT_NE(popped, nullptr);
    EXPECT_EQ(popped->type, model::EventType::Order);

    const auto* slot = std::get_if<model::OrderData*>(&popped->data);
    ASSERT_NE(slot, nullptr);
    ASSERT_NE(*slot, nullptr);
    EXPECT_EQ((*slot)->orderid, "OID-1");
    EXPECT_EQ((*slot)->symbol, "SPXW-TEST");

    order_pool.release(*slot);
    event_pool.release(popped);
}

} // namespace
