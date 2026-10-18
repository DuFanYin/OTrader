/**
 * The orders kept (BookkeepingEngine): a strategy's order is active until the venue ends it, a
 * cancel sent included; updates merge (an end is final, filled only rises, the legs stay as sent);
 * an end waits for its fills, or a timeout; a fill is kept once. What a strategy's active orders
 * would still trade is kept by contract, as they fill and end.
 */
#include "core/bookkeeping/bookkeeping_engine.hpp"
#include "model/constant.hpp"
#include "model/object.hpp"
#include <chrono>
#include <map>
#include <gtest/gtest.h>
#include <string>
#include <tuple>
#include <vector>

namespace {

using model::Order;
using model::Status;

/** An order of `quantity` of one contract, as sent. */
Order sent(const std::string& order_id, const std::string& symbol = "SPY", double quantity = 1) {
    Order o;
    o.order_id = order_id;
    o.symbol = symbol;
    o.quantity = quantity;
    o.legs = {{.ratio = 1, .symbol = symbol}};
    return o;
}

Order with(Order o, Status status, double filled = 0) {
    o.status = status;
    o.filled = filled;
    return o;
}

/** A combo order of two legs, A and B, sent by s1. */
Order sent_spread(core::BookkeepingEngine& ex) {
    Order o;
    o.order_id = "C1";
    o.symbol = "spread_A|B";
    o.quantity = 2;
    o.combo_type = model::ComboType::CUSTOM;
    for (const char* leg : {"A", "B"}) {
        o.legs.push_back({.ratio = 1, .symbol = leg});
    }
    ex.add_order("s1", o);
    return o;
}

model::Fill fill(const std::string& id, const std::string& symbol, double quantity,
                 const std::string& order_id = "C1") {
    model::Fill t;
    t.order_id = order_id;
    t.fill_id = id;
    t.symbol = symbol;
    t.quantity = quantity;
    return t;
}

TEST(BookkeepingOrders, AStrategysOrderIsActiveUntilTheVenueEndsIt) {
    core::BookkeepingEngine ex;
    ex.add_order("s1", sent("OID-1"));
    ex.add_order("s1", sent("OID-2"));
    EXPECT_EQ(ex.strategy_of("OID-1"), "s1");
    EXPECT_TRUE(ex.is_active("OID-1")) << "sent: active before the venue answers";
    EXPECT_TRUE(ex.has_active_order("s1"));

    EXPECT_TRUE(ex.update_order(with(sent("OID-1"), Status::WORKING), {}));
    EXPECT_TRUE(ex.update_order(with(sent("OID-1"), Status::PARTLY_FILLED), {}));
    EXPECT_TRUE(ex.is_active("OID-1"));
    EXPECT_TRUE(ex.update_order(with(sent("OID-1"), Status::FILLED), {}));
    EXPECT_FALSE(ex.is_active("OID-1"));
    EXPECT_TRUE(ex.update_order(with(sent("OID-2"), Status::CANCELLED), {}));
    EXPECT_FALSE(ex.is_active("OID-2"));
    EXPECT_FALSE(ex.has_active_order("s1"));
    EXPECT_EQ(ex.strategy_of("OID-2"), "s1") << "its strategy stays known";
}

TEST(BookkeepingOrders, AnOrderWithACancelSentIsActiveUntilTheVenueEndsIt) {
    core::BookkeepingEngine ex;
    ex.add_order("s1", sent("OID-1", "SPY", 2));
    EXPECT_TRUE(ex.cancel_sent("OID-1"));
    EXPECT_FALSE(ex.cancel_sent("OID-1")) << "one cancel an order";
    EXPECT_TRUE(ex.is_active("OID-1")) << "it may still fill";

    EXPECT_TRUE(ex.book_fill(fill("T1", "SPY", 1, "OID-1")));
    EXPECT_TRUE(ex.update_order(with(sent("OID-1", "SPY", 2), Status::CANCELLED, 1), {}));
    EXPECT_FALSE(ex.is_active("OID-1"));
    EXPECT_EQ(ex.find_order("OID-1")->filled, 1);
    EXPECT_FALSE(ex.cancel_sent("OID-1")) << "ended: nothing to cancel";
    EXPECT_FALSE(ex.cancel_sent("NONE"));
}

TEST(BookkeepingOrders, ActiveOrdersAreListedByStrategy) {
    core::BookkeepingEngine ex;
    ex.add_order("s1", sent("B"));
    ex.add_order("s1", sent("A"));
    ex.add_order("s2", sent("C"));
    const auto ids = [&](const std::string& strategy) {
        std::vector<std::string> out;
        ex.for_each_active_order(strategy, [&](const Order& o) { out.push_back(o.order_id); });
        return out;
    };
    EXPECT_EQ(ids("s1"), (std::vector<std::string>{"A", "B"}));
    EXPECT_EQ(ids(""), (std::vector<std::string>{"A", "B", "C"}));
}

TEST(BookkeepingOrders, RemovingAStrategyForgetsItsOrdersAndFills) {
    core::BookkeepingEngine ex;
    ex.add_order("s1", sent("OID-1"));
    ex.add_order("s2", sent("OID-3"));
    ex.book_fill(fill("T1", "SPY", 1, "OID-1"));
    ex.book_fill(fill("T3", "SPY", 1, "OID-3"));

    ex.remove_strategy("s1");
    EXPECT_EQ(ex.find_order("OID-1"), nullptr);
    EXPECT_EQ(ex.find_fill("T1"), nullptr);
    EXPECT_FALSE(ex.is_active("OID-1"));
    EXPECT_TRUE(ex.is_active("OID-3"));
    EXPECT_NE(ex.find_fill("T3"), nullptr);
}

TEST(BookkeepingOrders, AnOrderWithoutAnIdIsNotKept) {
    core::BookkeepingEngine ex;
    ex.add_order("s1", sent(""));
    EXPECT_EQ(ex.order_count(), 0U);
    EXPECT_FALSE(ex.has_active_order("s1"));
}

TEST(BookkeepingOrders, AnotherClientsOrderIsKeptAsReportedButNeverActiveHere) {
    core::BookkeepingEngine ex;
    const auto known = ex.update_order(with(sent("X1"), Status::WORKING), {});
    ASSERT_TRUE(known);
    EXPECT_EQ(known->order_id, "X1");
    EXPECT_TRUE(ex.strategy_of("X1").empty());
    EXPECT_FALSE(ex.is_active("X1"));
    EXPECT_FALSE(ex.cancel_sent("X1"));
}

TEST(BookkeepingOrders, ARestoredOrderIsActiveOnlyIfItWas) {
    core::BookkeepingEngine ex;
    ex.add_order("s1", with(sent("D1"), Status::FILLED, 1));
    EXPECT_FALSE(ex.is_active("D1"));
    EXPECT_FALSE(ex.has_active_order("s1"));
    EXPECT_EQ(ex.strategy_of("D1"), "s1");
    ex.add_order("s1", with(sent("W1"), Status::WORKING));
    EXPECT_TRUE(ex.is_active("W1"));
    EXPECT_TRUE(ex.has_active_order("s1"));
}

TEST(BookkeepingOrders, AFillIsKeptOnce) {
    core::BookkeepingEngine ex;
    ex.add_order("s1", sent("OID-1"));
    EXPECT_TRUE(ex.book_fill(fill("T1", "SPY", 1, "OID-1")));
    EXPECT_FALSE(ex.book_fill(fill("T1", "SPY", 1, "OID-1"))) << "reported again";
    EXPECT_EQ(ex.fills().size(), 1U);
}

TEST(BookkeepingTerminalUpdates, AnEndWaitsUntilEveryLegIsBooked) {
    core::BookkeepingEngine ex;
    const Order spread = sent_spread(ex);
    EXPECT_FALSE(ex.update_order(with(spread, Status::FILLED, 2), {})) << "held back";
    EXPECT_TRUE(ex.is_active("C1"));
    ex.book_fill(fill("t1", "A", 2));
    EXPECT_FALSE(ex.take_released(fill("t1", "A", 2))) << "leg B not booked";
    ex.book_fill(fill("t2", "B", 2));
    const auto end = ex.take_released(fill("t2", "B", 2));
    ASSERT_TRUE(end);
    EXPECT_EQ(end->status, Status::FILLED);
    EXPECT_FALSE(ex.is_active("C1"));
    EXPECT_FALSE(ex.take_released(fill("t2", "B", 2))) << "released once";
}

TEST(BookkeepingTerminalUpdates, AnEndWhoseFillsAreBookedIsToldAtOnce) {
    core::BookkeepingEngine ex;
    const Order spread = sent_spread(ex);
    ex.book_fill(fill("t1", "B", 2));
    ex.book_fill(fill("t2", "A", 2));
    EXPECT_TRUE(ex.update_order(with(spread, Status::FILLED, 2), {}));
}

TEST(BookkeepingTerminalUpdates, RepeatedAndOutOfDateUpdatesAreIgnored) {
    core::BookkeepingEngine ex;
    const Order spread = sent_spread(ex);
    const Order part = with(spread, Status::PARTLY_FILLED, 1);
    ex.book_fill(fill("t1", "A", 2));
    ex.book_fill(fill("t2", "B", 2));
    EXPECT_TRUE(ex.update_order(part, {}));
    EXPECT_FALSE(ex.update_order(part, {})) << "a repeat";
    EXPECT_TRUE(ex.update_order(with(spread, Status::FILLED, 2), {}));
    EXPECT_FALSE(ex.update_order(part, {})) << "out of date";
    EXPECT_EQ(ex.find_order("C1")->status, Status::FILLED);
    EXPECT_EQ(ex.find_order("C1")->filled, 2);
    EXPECT_EQ(ex.find_order("C1")->legs.size(), 2U) << "its legs as sent";
}

TEST(BookkeepingTerminalUpdates, AnEndHeldBackIsToldAfterTheTimeoutOnce) {
    core::BookkeepingEngine ex;
    const Order spread = sent_spread(ex);
    const model::DateTime t0{};
    ASSERT_FALSE(ex.update_order(with(spread, Status::CANCELLED, 1), t0));
    ASSERT_FALSE(
        ex.update_order(with(spread, Status::FILLED, 2), t0 + std::chrono::seconds(5)))
        << "a repeat replaces it; the time held is the first's";
    EXPECT_TRUE(ex.take_overdue(t0 + std::chrono::seconds(9)).empty());
    const auto due = ex.take_overdue(t0 + core::BookkeepingEngine::kTerminalUpdateTimeout);
    ASSERT_EQ(due.size(), 1U);
    EXPECT_EQ(due[0].order_id, "C1");
    EXPECT_EQ(due[0].status, Status::FILLED);
    EXPECT_EQ(due[0].filled, 2);
    EXPECT_TRUE(ex.take_overdue(t0 + std::chrono::hours(1)).empty()) << "told once";
}

TEST(BookkeepingTerminalUpdates, OnlyAStrategysEndWithFillsToComeIsHeldBack) {
    core::BookkeepingEngine ex;
    const Order spread = sent_spread(ex);
    EXPECT_TRUE(ex.update_order(with(spread, Status::PARTLY_FILLED, 1), {})) << "not an end";
    EXPECT_TRUE(ex.update_order(with(sent("X1"), Status::FILLED, 1), {}))
        << "another client's";
    core::BookkeepingEngine fresh;
    fresh.add_order("s1", spread);
    EXPECT_TRUE(fresh.update_order(with(spread, Status::CANCELLED), {}))
        << "nothing filled: no fill to wait for";
}

TEST(BookkeepingTerminalUpdates, AFillReportedAfterTheEndRaisesWhatWasTradedNotTheStatus) {
    core::BookkeepingEngine ex;
    ex.add_order("s1", sent("O1", "SPY", 2));
    ex.book_fill(fill("t1", "SPY", 1, "O1"));
    ASSERT_TRUE(ex.update_order(with(sent("O1", "SPY", 2), Status::CANCELLED, 1), {}));
    const auto late =
        ex.update_order(with(sent("O1", "SPY", 2), Status::FILLED, 2), {});
    ASSERT_TRUE(late);
    EXPECT_EQ(late->status, Status::CANCELLED); // an end is final
    EXPECT_EQ(late->filled, 2);
    EXPECT_FALSE(ex.update_order(with(sent("O1", "SPY", 2), Status::FILLED, 2), {}));
}

TEST(BookkeepingTerminalUpdates, AContractsOwnOrderWaitsForItsOwnFills) {
    core::BookkeepingEngine ex;
    ex.add_order("s1", sent("O1", "SPY", 3));
    EXPECT_FALSE(ex.update_order(with(sent("O1", "SPY", 3), Status::FILLED, 3), {}));
    ex.book_fill(fill("t1", "SPY", 2, "O1"));
    EXPECT_FALSE(ex.take_released(fill("t1", "SPY", 2, "O1")));
    ex.book_fill(fill("t2", "SPY", 1, "O1"));
    EXPECT_TRUE(ex.take_released(fill("t2", "SPY", 1, "O1")));
}

TEST(BookkeepingForgetting, OnlyOrdersDoneWithAndTheirFillsAreForgotten) {
    core::BookkeepingEngine ex;
    ex.add_order("s1", sent("DONE"));
    ex.add_order("s1", sent("OPEN"));
    ex.book_fill(fill("T1", "SPY", 1, "DONE"));
    ex.update_order(with(sent("DONE"), Status::FILLED, 1), {});
    ex.forget_finished();
    EXPECT_EQ(ex.find_fill("T1"), nullptr);
    EXPECT_TRUE(ex.fills().empty());
    EXPECT_EQ(ex.find_order("DONE"), nullptr);
    EXPECT_NE(ex.find_order("OPEN"), nullptr);
}

/** What the strategy's active orders would still trade, by contract. */
std::map<std::string, double> working(const core::BookkeepingEngine& ex, const std::string& s) {
    std::map<std::string, double> out;
    ex.for_each_working(s, [&](const std::string& symbol, double q) { out[symbol] = q; });
    return out;
}

TEST(BookkeepingWorking, AStrategysActiveOrdersCountByContractAsTheyFillAndEnd) {
    core::BookkeepingEngine ex;
    Order spread = sent("C1", "spread_A|B", 2);
    spread.legs = {{.ratio = 1, .side = model::Side::BUY, .symbol = "A"},
                   {.ratio = 2, .side = model::Side::SELL, .symbol = "B"}};
    ex.add_order("s1", spread);
    ex.add_order("s2", sent("O1", "A", 5)); // another strategy's: not s1's
    EXPECT_EQ(working(ex, "s1"), (std::map<std::string, double>{{"A", 2}, {"B", -4}}));
    EXPECT_EQ(working(ex, "s2"), (std::map<std::string, double>{{"A", 5}}));

    ex.update_order(with(spread, Status::PARTLY_FILLED, 1), {});
    EXPECT_EQ(working(ex, "s1"), (std::map<std::string, double>{{"A", 1}, {"B", -2}}));
    for (const auto& [symbol, side, quantity] :
         {std::tuple{"A", model::Side::BUY, 1.0}, std::tuple{"B", model::Side::SELL, 2.0}}) {
        model::Fill fill;
        fill.fill_id = std::string("F") + symbol;
        fill.order_id = "C1";
        fill.symbol = symbol;
        fill.side = side;
        fill.quantity = quantity;
        ex.book_fill(fill);
    }
    ex.update_order(with(spread, Status::CANCELLED, 1), {}); // its fills in: the end is told
    EXPECT_TRUE(working(ex, "s1").empty());
    EXPECT_FALSE(ex.has_active_order("s1"));
    EXPECT_TRUE(ex.has_active_order("s2"));

    ex.add_order("s2", sent("O1", "B", 3)); // the same id again: the one before no longer counts
    EXPECT_EQ(working(ex, "s2"), (std::map<std::string, double>{{"B", 3}}));
}

} // namespace
