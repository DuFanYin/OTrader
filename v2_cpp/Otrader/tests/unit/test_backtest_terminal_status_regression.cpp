#include "runtime/backtest/engine_main.hpp"
#include "utilities/event.hpp"
#include <gtest/gtest.h>

namespace {

using utilities::Event;
using utilities::EventType;
using utilities::Status;

TEST(BacktestDataflow, TerminalStatusDoesNotReAddActiveOnLaterNonTerminalUpdate) {
    backtest::MainEngine main;
    ASSERT_NE(main.execution_engine(), nullptr);

    main.execution_engine()->register_active_order("s1", "OID-1");

    auto* ord = main.acquire_order();
    ASSERT_NE(ord, nullptr);
    ord->orderid = "OID-1";
    ord->symbol = "AAPL.STK";
    ord->status = Status::ALLTRADED;
    main.put_event(Event(EventType::Order, ord));
    EXPECT_EQ(main.execution_engine()->active_order_ids().count("OID-1"), 0U);

    // A buggy gateway could send a late "NOTTRADED/PARTTRADED" status update after ALLTRADED.
    // Current contract: we must NOT re-add an order to active set via order events alone.
    auto* ord2 = main.acquire_order();
    ASSERT_NE(ord2, nullptr);
    ord2->orderid = "OID-1";
    ord2->symbol = "AAPL.STK";
    ord2->status = Status::PARTTRADED;
    main.put_event(Event(EventType::Order, ord2));
    EXPECT_EQ(main.execution_engine()->active_order_ids().count("OID-1"), 0U);
}

} // namespace

