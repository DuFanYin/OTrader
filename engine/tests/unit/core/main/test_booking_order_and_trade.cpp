#include "tests/lone_backtest.hpp"
#include "model/event.hpp"
#include <gtest/gtest.h>
 
namespace {
 
using model::Side;
using model::Event;
using model::EventType;
using model::Order;
using model::Status;
using model::Fill;
 
TEST(BacktestEventDispatch, OrderAndTradeUpdateExecutionAndPosition) {
    test::LoneBacktest backtest;
    core::Main& main = backtest.main();
    ASSERT_NE(main.bookkeeping_engine(), nullptr);
 
    // Pretend strategy submitted order earlier (so dispatch can resolve strategy_name).
    main.bookkeeping_engine()->add_order(
        "s1", {.symbol = "SPXW-20250804-C-5000",
               .order_id = "OID-1",
               .legs = {{.ratio = 1, .symbol = "SPXW-20250804-C-5000"}}});
 
    // Order event should be kept by the book and tracked.
    auto* ord = main.event_loop()->acquire_order();
    ASSERT_NE(ord, nullptr);
    ord->order_id = "OID-1";
    ord->symbol = "SPXW-20250804-C-5000";
    ord->status = Status::WORKING;
    main.event_loop()->put_event(Event(EventType::Order, ord));
 
    EXPECT_NE(main.bookkeeping_engine()->find_order("OID-1"), nullptr);
    EXPECT_EQ(main.bookkeeping_engine()->strategy_of("OID-1"), "s1");
    EXPECT_TRUE(main.bookkeeping_engine()->is_active("OID-1"));
 
    // Trade event should create holding for s1.
    auto* tr = main.event_loop()->acquire_fill();
    ASSERT_NE(tr, nullptr);
    tr->order_id = "OID-1";
    tr->fill_id = "TID-1";
    tr->symbol = "SPXW-20250804-C-5000";
    tr->side = Side::BUY;
    tr->price = 1.25;
    tr->quantity = 2;
    main.event_loop()->put_event(Event(EventType::Fill, tr));
 
    auto* holding = main.bookkeeping_engine()->find_holding("s1");
    ASSERT_NE(holding, nullptr);
    auto it = holding->options.find("SPXW-20250804-C-5000");
    ASSERT_NE(it, holding->options.end());
    EXPECT_EQ(it->second.quantity, 2);
    EXPECT_DOUBLE_EQ(it->second.avg_cost, 1.25);
 
    // Terminal order status should remove from active tracking.
    auto* ord2 = main.event_loop()->acquire_order();
    ASSERT_NE(ord2, nullptr);
    *ord2 = *main.bookkeeping_engine()->find_order("OID-1");
    ord2->status = Status::FILLED;
    main.event_loop()->put_event(Event(EventType::Order, ord2));
    EXPECT_FALSE(main.bookkeeping_engine()->is_active("OID-1"));
}
 
} // namespace

