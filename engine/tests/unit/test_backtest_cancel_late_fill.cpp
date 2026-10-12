#include "core/engine_backtest.hpp"
#include "model/event.hpp"
#include <gtest/gtest.h>

namespace {

using model::CancelRequest;
using model::Direction;
using model::Event;
using model::EventType;
using model::OrderData;
using model::Status;

TEST(BacktestDataflow, CancelRemovesActiveButLateFillStillUpdatesHolding) {
    core::backtest::MainEngine main;
    ASSERT_NE(main.execution_engine(), nullptr);
    ASSERT_NE(main.position_engine(), nullptr);

    main.execution_engine()->register_active_order("s1", "OID-1");

    // An order the engine has (its update came in as an event).
    auto* ord = main.acquire_order();
    ASSERT_NE(ord, nullptr);
    ord->orderid = "OID-1";
    ord->symbol = "AAPL";
    ord->status = Status::NOTTRADED;
    ord->volume = 10;
    main.put_event(Event(EventType::Order, ord));
    EXPECT_TRUE(main.execution_engine()->is_working("OID-1"));

    // Cancel: the engine stops tracking it as working.
    CancelRequest req;
    req.orderid = "OID-1";
    req.symbol = "AAPL";
    main.cancel_order(req);
    EXPECT_FALSE(main.execution_engine()->is_working("OID-1"));

    // Late fill after cancel: should still update holding (real exchanges can send this).
    auto* tr = main.acquire_trade();
    ASSERT_NE(tr, nullptr);
    tr->orderid = "OID-1";
    tr->tradeid = "TID-LATE";
    tr->symbol = "AAPL";
    tr->direction = Direction::LONG;
    tr->price = 101.0;
    tr->volume = 1;
    main.put_event(Event(EventType::Trade, tr));

    auto* h = main.get_holding("s1");
    ASSERT_NE(h, nullptr);
    EXPECT_EQ(h->underlyingPosition.quantity, 1);
    EXPECT_DOUBLE_EQ(h->underlyingPosition.avg_cost, 101.0);
}

} // namespace

