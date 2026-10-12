#include "core/engines/execution.hpp"
#include "model/constant.hpp"
#include "model/object.hpp"
#include <gtest/gtest.h>
 
namespace {
 
using model::OrderData;
using model::Status;
 
TEST(ExecutionEngineTracking, RegisterAndRemoveOnTerminalStatuses) {
    core::ExecutionEngine ex;
 
    ex.register_active_order("s1", "OID-1");
    ex.register_active_order("s1", "OID-2");
 
    EXPECT_EQ(ex.get_strategy_name_for_order("OID-1"), "s1");
    EXPECT_EQ(ex.get_strategy_name_for_order("OID-2"), "s1");
    EXPECT_TRUE(ex.is_working("OID-1"));
    EXPECT_TRUE(ex.is_working("OID-2"));
 
    OrderData o;
    o.orderid = "OID-1";
    o.symbol = "SYM";
    o.status = Status::NOTTRADED;
    ex.process_order_event("s1", o);
    EXPECT_TRUE(ex.is_working("OID-1"));
 
    o.status = Status::PARTTRADED;
    ex.process_order_event("s1", o);
    EXPECT_TRUE(ex.is_working("OID-1"));
 
    o.status = Status::ALLTRADED;
    ex.process_order_event("s1", o);
    EXPECT_FALSE(ex.is_working("OID-1"));
 
    o.orderid = "OID-2";
    o.status = Status::CANCELLED;
    ex.process_order_event("s1", o);
    EXPECT_FALSE(ex.is_working("OID-2"));
}
 
TEST(ExecutionEngineTracking, ACancelledOrderStopsBeingTrackedButKeepsItsStrategy) {
    core::ExecutionEngine ex;
    ex.register_active_order("s1", "OID-1");
 
    ex.remove_order_tracking("OID-1"); // Engine::cancel_order, once the venue has it
 
    EXPECT_FALSE(ex.is_working("OID-1"));
    EXPECT_EQ(ex.active_orders("s1").count("OID-1"), 0U);
    // Its last status and a fill that comes late still go to the strategy.
    EXPECT_EQ(ex.get_strategy_name_for_order("OID-1"), "s1");
}
 
TEST(ExecutionEngineTracking, RemoveStrategyTrackingClearsReverseMap) {
    core::ExecutionEngine ex;
    ex.register_active_order("s1", "OID-1");
    ex.register_active_order("s1", "OID-2");
    ex.register_active_order("s2", "OID-3");
 
    ex.remove_strategy_tracking("s1");
 
    EXPECT_TRUE(ex.get_strategy_name_for_order("OID-1").empty());
    EXPECT_TRUE(ex.get_strategy_name_for_order("OID-2").empty());
    EXPECT_EQ(ex.get_strategy_name_for_order("OID-3"), "s2");
    EXPECT_FALSE(ex.is_working("OID-1"));
    EXPECT_FALSE(ex.is_working("OID-2"));
    EXPECT_TRUE(ex.is_working("OID-3"));
}
 
/** A combo order of two legs, sent by s1 and kept as sent. */
OrderData sent_spread(core::ExecutionEngine& ex) {
    OrderData o;
    o.orderid = "C1";
    o.symbol = "spread_A|B";
    o.is_combo = true;
    o.volume = 2;
    for (const char* leg : {"A", "B"}) {
        model::Leg l;
        l.symbol = leg;
        l.ratio = 1;
        o.legs.push_back(l);
    }
    ex.register_sent_order("s1", o);
    return o;
}

model::TradeData fill(const std::string& id, const std::string& symbol, double volume) {
    model::TradeData t;
    t.orderid = "C1";
    t.tradeid = id;
    t.symbol = symbol;
    t.volume = volume;
    return t;
}

OrderData ended(OrderData o, double traded) {
    o.status = Status::ALLTRADED;
    o.traded = traded;
    return o;
}

TEST(ExecutionEngineTerminalUpdates, DeferredUntilEveryLegIsBooked) {
    core::ExecutionEngine ex;
    const OrderData sent = sent_spread(ex);
    EXPECT_TRUE(ex.defer_terminal_update(ended(sent, 2), {}));
    ex.process_trade_event(fill("t1", "A", 2));
    EXPECT_FALSE(ex.take_ready_terminal_update(fill("t1", "A", 2))) << "leg B not booked";
    ex.process_trade_event(fill("t2", "B", 2));
    const auto end = ex.take_ready_terminal_update(fill("t2", "B", 2));
    ASSERT_TRUE(end);
    EXPECT_EQ(end->status, Status::ALLTRADED);
    EXPECT_TRUE(ex.process_order_event("s1", *end) != nullptr);
    EXPECT_FALSE(ex.is_working("C1"));
}

TEST(ExecutionEngineTerminalUpdates, NotDeferredWhenFillsAreBooked) {
    core::ExecutionEngine ex;
    const OrderData sent = sent_spread(ex);
    ex.process_trade_event(fill("t1", "B", 2));
    ex.process_trade_event(fill("t2", "A", 2));
    EXPECT_FALSE(ex.defer_terminal_update(ended(sent, 2), {}));
}

TEST(ExecutionEngineTerminalUpdates, RepeatedAndOutOfDateUpdatesAreIgnored) {
    core::ExecutionEngine ex;
    OrderData sent = sent_spread(ex);
    OrderData part = sent;
    part.status = Status::PARTTRADED;
    part.traded = 1;
    ASSERT_NE(ex.process_order_event("s1", part), nullptr);
    EXPECT_EQ(ex.process_order_event("s1", part), nullptr) << "a repeat";
    ASSERT_NE(ex.process_order_event("s1", ended(sent, 2)), nullptr);
    EXPECT_EQ(ex.process_order_event("s1", part), nullptr) << "out of date";
    EXPECT_EQ(ex.get_order("C1")->status, Status::ALLTRADED);
    EXPECT_EQ(ex.get_order("C1")->traded, 2);
    EXPECT_EQ(ex.get_order("C1")->legs.size(), 2U) << "its legs as sent";
}

TEST(ExecutionEngineTerminalUpdates, ExpiresAfterTimeout) {
    core::ExecutionEngine ex;
    const OrderData sent = sent_spread(ex);
    const model::DateTime t0{};
    ASSERT_TRUE(ex.defer_terminal_update(ended(sent, 2), t0));
    EXPECT_TRUE(ex.take_expired_terminal_updates(t0 + std::chrono::seconds(9)).empty());
    const auto due =
        ex.take_expired_terminal_updates(t0 + core::ExecutionEngine::kTerminalUpdateTimeout);
    ASSERT_EQ(due.size(), 1U);
    EXPECT_EQ(due[0].orderid, "C1");
    EXPECT_TRUE(ex.take_expired_terminal_updates(t0 + std::chrono::hours(1)).empty())
        << "returned once";
}

} // namespace

