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

TEST(BacktestDataflow, TradeBeforeOrderWithStrategyMappingStillUpdatesHolding) {
    test::LoneBacktest backtest;
    core::Main& main = backtest.main();
    ASSERT_NE(main.bookkeeping_engine(), nullptr);

    // Critical: map order_id -> strategy first, otherwise dispatch_trade may not resolve strategy.
    main.bookkeeping_engine()->add_order(
        "s1", {.symbol = "AAPL", .order_id = "OID-1", .legs = {{.ratio = 1, .symbol = "AAPL"}}});

    // Trade arrives before any Order update.
    auto* tr = main.event_loop()->acquire_fill();
    ASSERT_NE(tr, nullptr);
    tr->order_id = "OID-1";
    tr->fill_id = "TID-1";
    tr->symbol = "AAPL";
    tr->side = Side::BUY;
    tr->price = 100.0;
    tr->quantity = 3;
    main.event_loop()->put_event(Event(EventType::Fill, tr));

    auto* h = main.bookkeeping_engine()->find_holding("s1");
    ASSERT_NE(h, nullptr);
    EXPECT_EQ(h->shares.quantity, 3);
    EXPECT_DOUBLE_EQ(h->shares.avg_cost, 100.0);

    // Then Order update arrives; should not break holding.
    auto* ord = main.event_loop()->acquire_order();
    ASSERT_NE(ord, nullptr);
    ord->order_id = "OID-1";
    ord->symbol = "AAPL";
    ord->status = Status::PARTLY_FILLED;
    ord->filled = 3;
    ord->quantity = 5;
    main.event_loop()->put_event(Event(EventType::Order, ord));

    auto* h2 = main.bookkeeping_engine()->find_holding("s1");
    ASSERT_NE(h2, nullptr);
    EXPECT_EQ(h2->shares.quantity, 3);
}

TEST(BacktestDataflow, OrderBeforeTradeStillUpdatesExecutionCache) {
    test::LoneBacktest backtest;
    core::Main& main = backtest.main();
    ASSERT_NE(main.bookkeeping_engine(), nullptr);

    main.bookkeeping_engine()->add_order(
        "s1", {.symbol = "SPXW-20250804-C-5000",
               .order_id = "OID-2",
               .legs = {{.ratio = 1, .symbol = "SPXW-20250804-C-5000"}}});

    auto* ord = main.event_loop()->acquire_order();
    ASSERT_NE(ord, nullptr);
    ord->order_id = "OID-2";
    ord->symbol = "SPXW-20250804-C-5000";
    ord->status = Status::WORKING;
    ord->quantity = 2;
    main.event_loop()->put_event(Event(EventType::Order, ord));

    auto* cached = main.bookkeeping_engine()->find_order("OID-2");
    ASSERT_NE(cached, nullptr);
    EXPECT_EQ(cached->status, Status::WORKING);
    EXPECT_TRUE(main.bookkeeping_engine()->is_active("OID-2"));
}

} // namespace

