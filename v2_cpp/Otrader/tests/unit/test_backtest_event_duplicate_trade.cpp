#include "runtime/backtest/engine_main.hpp"
#include "utilities/event.hpp"
#include <gtest/gtest.h>

namespace {

using utilities::Direction;
using utilities::Event;
using utilities::EventType;
using utilities::TradeData;

TEST(BacktestDataflow, DuplicateTradeIdIsIdempotentAcrossDispatch) {
    backtest::MainEngine main;
    ASSERT_NE(main.execution_engine(), nullptr);
    ASSERT_NE(main.position_engine(), nullptr);

    main.execution_engine()->register_active_order("s1", "OID-1");

    auto push_trade = [&](const char* tid) {
        auto* tr = main.acquire_trade();
        ASSERT_NE(tr, nullptr);
        tr->orderid = "OID-1";
        tr->tradeid = tid;
        tr->symbol = "AAPL.STK";
        tr->direction = Direction::LONG;
        tr->price = 100.0;
        tr->volume = 2;
        main.put_event(Event(EventType::Trade, tr));
    };

    push_trade("TID-1");
    push_trade("TID-1"); // duplicate

    auto* h = main.get_holding("s1");
    ASSERT_NE(h, nullptr);
    EXPECT_EQ(h->underlyingPosition.quantity, 2);

    // ExecutionEngine stores trades by tradeid; duplicate should overwrite, not grow.
    auto trades = main.execution_engine()->get_all_trades();
    EXPECT_EQ(trades.size(), 1U);
    EXPECT_EQ(trades[0].tradeid, "TID-1");
}

} // namespace

