#include "tests/lone_backtest.hpp"
#include "model/event.hpp"
#include <gtest/gtest.h>

namespace {

using model::CancelRequest;
using model::Side;
using model::Event;
using model::EventType;
using model::Order;
using model::Status;

TEST(BacktestDataflow, AnOrderWithACancelSentStaysActiveAndALateFillStillUpdatesHolding) {
    test::LoneBacktest backtest;
    core::Main& main = backtest.main();
    ASSERT_NE(main.bookkeeping_engine(), nullptr);

    main.bookkeeping_engine()->add_order(
        "s1", {.symbol = "AAPL", .order_id = "OID-1", .legs = {{.ratio = 1, .symbol = "AAPL"}}});

    // An order the engine has (its update came in as an event).
    auto* ord = main.event_loop()->acquire_order();
    ASSERT_NE(ord, nullptr);
    ord->order_id = "OID-1";
    ord->symbol = "AAPL";
    ord->status = Status::WORKING;
    ord->quantity = 10;
    main.event_loop()->put_event(Event(EventType::Order, ord));
    EXPECT_TRUE(main.bookkeeping_engine()->is_active("OID-1"));

    // A cancel sent: the order stays active until the venue ends it (this one never had it).
    CancelRequest req;
    req.order_id = "OID-1";
    req.symbol = "AAPL";
    main.cancel_order(req);
    EXPECT_TRUE(main.bookkeeping_engine()->is_active("OID-1"));

    // Late fill after cancel: should still update holding (real exchanges can send this).
    auto* tr = main.event_loop()->acquire_fill();
    ASSERT_NE(tr, nullptr);
    tr->order_id = "OID-1";
    tr->fill_id = "TID-LATE";
    tr->symbol = "AAPL";
    tr->side = Side::BUY;
    tr->price = 101.0;
    tr->quantity = 1;
    main.event_loop()->put_event(Event(EventType::Fill, tr));

    auto* h = main.bookkeeping_engine()->find_holding("s1");
    ASSERT_NE(h, nullptr);
    EXPECT_EQ(h->shares.quantity, 1);
    EXPECT_DOUBLE_EQ(h->shares.avg_cost, 101.0);
}

} // namespace

