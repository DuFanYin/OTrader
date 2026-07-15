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

TEST(BacktestDataflow, TradeBeforeOrderWithStrategyMappingStillUpdatesHolding) {
    backtest::MainEngine main;
    ASSERT_NE(main.execution_engine(), nullptr);
    ASSERT_NE(main.position_engine(), nullptr);

    // Critical: map orderid -> strategy first, otherwise dispatch_trade may not resolve strategy.
    main.execution_engine()->register_active_order("s1", "OID-1");

    // Trade arrives before any Order update.
    auto* tr = main.acquire_trade();
    ASSERT_NE(tr, nullptr);
    tr->orderid = "OID-1";
    tr->tradeid = "TID-1";
    tr->symbol = "AAPL.STK";
    tr->direction = Direction::LONG;
    tr->price = 100.0;
    tr->volume = 3;
    main.put_event(Event(EventType::Trade, tr));

    auto* h = main.get_holding("s1");
    ASSERT_NE(h, nullptr);
    EXPECT_EQ(h->underlyingPosition.quantity, 3);
    EXPECT_DOUBLE_EQ(h->underlyingPosition.avg_cost, 100.0);

    // Then Order update arrives; should not break holding.
    auto* ord = main.acquire_order();
    ASSERT_NE(ord, nullptr);
    ord->orderid = "OID-1";
    ord->symbol = "AAPL.STK";
    ord->status = Status::PARTTRADED;
    ord->traded = 3;
    ord->volume = 5;
    main.put_event(Event(EventType::Order, ord));

    auto* h2 = main.get_holding("s1");
    ASSERT_NE(h2, nullptr);
    EXPECT_EQ(h2->underlyingPosition.quantity, 3);
}

TEST(BacktestDataflow, OrderBeforeTradeStillUpdatesExecutionCache) {
    backtest::MainEngine main;
    ASSERT_NE(main.execution_engine(), nullptr);

    main.execution_engine()->register_active_order("s1", "OID-2");

    auto* ord = main.acquire_order();
    ASSERT_NE(ord, nullptr);
    ord->orderid = "OID-2";
    ord->symbol = "SPXW_20250804C05000000";
    ord->status = Status::NOTTRADED;
    ord->volume = 2;
    main.put_event(Event(EventType::Order, ord));

    auto* cached = main.execution_engine()->get_order("OID-2");
    ASSERT_NE(cached, nullptr);
    EXPECT_EQ(cached->status, Status::NOTTRADED);
    EXPECT_EQ(main.execution_engine()->active_order_ids().count("OID-2"), 1U);
}

} // namespace

