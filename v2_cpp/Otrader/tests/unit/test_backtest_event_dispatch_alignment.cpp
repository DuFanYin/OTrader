#include "runtime/backtest/engine_main.hpp"
#include "utilities/event.hpp"
#include <gtest/gtest.h>
 
namespace {
 
using utilities::Direction;
using utilities::Event;
using utilities::EventType;
using utilities::OrderData;
using utilities::Status;
using utilities::TradeData;
 
TEST(BacktestEventDispatch, OrderAndTradeUpdateExecutionAndPosition) {
    backtest::MainEngine main;
    ASSERT_NE(main.execution_engine(), nullptr);
    ASSERT_NE(main.position_engine(), nullptr);
 
    // Pretend strategy submitted order earlier (so dispatch can resolve strategy_name).
    main.execution_engine()->register_active_order("s1", "OID-1");
 
    // Order event should be cached by ExecutionEngine and tracked.
    auto* ord = main.acquire_order();
    ASSERT_NE(ord, nullptr);
    ord->orderid = "OID-1";
    ord->symbol = "SPXW_20250804C05000000";
    ord->status = Status::NOTTRADED;
    main.put_event(Event(EventType::Order, ord));
 
    EXPECT_NE(main.execution_engine()->get_order("OID-1"), nullptr);
    EXPECT_EQ(main.execution_engine()->get_strategy_name_for_order("OID-1"), "s1");
    EXPECT_EQ(main.execution_engine()->active_order_ids().count("OID-1"), 1U);
 
    // Trade event should create holding for s1.
    auto* tr = main.acquire_trade();
    ASSERT_NE(tr, nullptr);
    tr->orderid = "OID-1";
    tr->tradeid = "TID-1";
    tr->symbol = "SPXW_20250804C05000000";
    tr->direction = Direction::LONG;
    tr->price = 1.25;
    tr->volume = 2;
    main.put_event(Event(EventType::Trade, tr));
 
    auto* holding = main.get_holding("s1");
    ASSERT_NE(holding, nullptr);
    auto it = holding->optionPositions.find("SPXW_20250804C05000000");
    ASSERT_NE(it, holding->optionPositions.end());
    EXPECT_EQ(it->second.quantity, 2);
    EXPECT_DOUBLE_EQ(it->second.avg_cost, 1.25);
 
    // Terminal order status should remove from active tracking.
    auto* ord2 = main.acquire_order();
    ASSERT_NE(ord2, nullptr);
    *ord2 = *main.execution_engine()->get_order("OID-1");
    ord2->status = Status::ALLTRADED;
    main.put_event(Event(EventType::Order, ord2));
    EXPECT_EQ(main.execution_engine()->active_order_ids().count("OID-1"), 0U);
}
 
} // namespace

