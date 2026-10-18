#include "tests/lone_backtest.hpp"
#include "model/event.hpp"
#include <gtest/gtest.h>

namespace {

using model::Event;
using model::EventType;
using model::Status;

TEST(BacktestDataflow, TerminalStatusDoesNotReAddActiveOnLaterNonTerminalUpdate) {
    test::LoneBacktest backtest;
    core::Main& main = backtest.main();
    ASSERT_NE(main.bookkeeping_engine(), nullptr);

    main.bookkeeping_engine()->add_order(
        "s1", {.symbol = "AAPL", .order_id = "OID-1", .legs = {{.ratio = 1, .symbol = "AAPL"}}});

    auto* ord = main.event_loop()->acquire_order();
    ASSERT_NE(ord, nullptr);
    ord->order_id = "OID-1";
    ord->symbol = "AAPL";
    ord->status = Status::FILLED;
    main.event_loop()->put_event(Event(EventType::Order, ord));
    EXPECT_FALSE(main.bookkeeping_engine()->is_active("OID-1"));

    // A buggy gateway could send a late "WORKING/PARTLY_FILLED" status update after FILLED.
    // Current contract: we must NOT re-add an order to active set via order events alone.
    auto* ord2 = main.event_loop()->acquire_order();
    ASSERT_NE(ord2, nullptr);
    ord2->order_id = "OID-1";
    ord2->symbol = "AAPL";
    ord2->status = Status::PARTLY_FILLED;
    main.event_loop()->put_event(Event(EventType::Order, ord2));
    EXPECT_FALSE(main.bookkeeping_engine()->is_active("OID-1"));
}

} // namespace

