#include "runtime/backtest/engine_main.hpp"
#include "utilities/event.hpp"
#include <gtest/gtest.h>

namespace {

using utilities::CancelRequest;
using utilities::Direction;
using utilities::Event;
using utilities::EventType;
using utilities::OrderData;
using utilities::Status;

TEST(BacktestDataflow, CancelRemovesActiveButLateFillStillUpdatesHolding) {
    backtest::MainEngine main;
    ASSERT_NE(main.execution_engine(), nullptr);
    ASSERT_NE(main.position_engine(), nullptr);

    main.execution_engine()->register_active_order("s1", "OID-1");

    // Seed an order in cache so cancel_impl can mark it CANCELLED and emit an Order event.
    auto* ord = main.acquire_order();
    ASSERT_NE(ord, nullptr);
    ord->orderid = "OID-1";
    ord->symbol = "AAPL.STK";
    ord->status = Status::NOTTRADED;
    ord->volume = 10;
    main.put_event(Event(EventType::Order, ord));
    EXPECT_EQ(main.execution_engine()->active_order_ids().count("OID-1"), 1U);

    // Cancel (will remove tracking; and also emit CANCELLED Order event via cancel_impl).
    CancelRequest req;
    req.orderid = "OID-1";
    req.symbol = "AAPL.STK";
    main.cancel_order(req);
    EXPECT_EQ(main.execution_engine()->active_order_ids().count("OID-1"), 0U);

    // Late fill after cancel: should still update holding (real exchanges can send this).
    auto* tr = main.acquire_trade();
    ASSERT_NE(tr, nullptr);
    tr->orderid = "OID-1";
    tr->tradeid = "TID-LATE";
    tr->symbol = "AAPL.STK";
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

