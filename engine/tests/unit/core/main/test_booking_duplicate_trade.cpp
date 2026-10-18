#include "tests/lone_backtest.hpp"
#include "model/event.hpp"
#include <gtest/gtest.h>

namespace {

using model::Side;
using model::Event;
using model::EventType;
using model::Fill;

TEST(BacktestDataflow, DuplicateTradeIdIsIdempotentAcrossDispatch) {
    test::LoneBacktest backtest;
    core::Main& main = backtest.main();
    ASSERT_NE(main.bookkeeping_engine(), nullptr);

    main.bookkeeping_engine()->add_order(
        "s1", {.symbol = "AAPL", .order_id = "OID-1", .legs = {{.ratio = 1, .symbol = "AAPL"}}});

    auto push_trade = [&](const char* tid) {
        auto* tr = main.event_loop()->acquire_fill();
        ASSERT_NE(tr, nullptr);
        tr->order_id = "OID-1";
        tr->fill_id = tid;
        tr->symbol = "AAPL";
        tr->side = Side::BUY;
        tr->price = 100.0;
        tr->quantity = 2;
        main.event_loop()->put_event(Event(EventType::Fill, tr));
    };

    push_trade("TID-1");
    push_trade("TID-1"); // duplicate

    auto* h = main.bookkeeping_engine()->find_holding("s1");
    ASSERT_NE(h, nullptr);
    EXPECT_EQ(h->shares.quantity, 2);

    // The order book keeps a fill once, by its fill_id.
    EXPECT_EQ(main.bookkeeping_engine()->fills().size(), 1U);
    EXPECT_NE(main.bookkeeping_engine()->find_fill("TID-1"), nullptr);
}

} // namespace

