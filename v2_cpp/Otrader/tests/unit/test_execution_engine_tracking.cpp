#include "core/engine_execution.hpp"
#include "utilities/constant.hpp"
#include "utilities/object.hpp"
#include <gtest/gtest.h>
 
namespace {
 
using utilities::OrderData;
using utilities::Status;
 
TEST(ExecutionEngineTracking, RegisterAndRemoveOnTerminalStatuses) {
    core::ExecutionEngine ex;
    ex.ensure_strategy_key("s1");
 
    ex.register_active_order("s1", "OID-1");
    ex.register_active_order("s1", "OID-2");
 
    EXPECT_EQ(ex.get_strategy_name_for_order("OID-1"), "s1");
    EXPECT_EQ(ex.get_strategy_name_for_order("OID-2"), "s1");
    EXPECT_EQ(ex.active_order_ids().count("OID-1"), 1U);
    EXPECT_EQ(ex.active_order_ids().count("OID-2"), 1U);
 
    OrderData o;
    o.orderid = "OID-1";
    o.symbol = "SYM";
    o.status = Status::NOTTRADED;
    ex.process_order_event("s1", o);
    EXPECT_EQ(ex.active_order_ids().count("OID-1"), 1U);
 
    o.status = Status::PARTTRADED;
    ex.process_order_event("s1", o);
    EXPECT_EQ(ex.active_order_ids().count("OID-1"), 1U);
 
    o.status = Status::ALLTRADED;
    ex.process_order_event("s1", o);
    EXPECT_EQ(ex.active_order_ids().count("OID-1"), 0U);
 
    o.orderid = "OID-2";
    o.status = Status::CANCELLED;
    ex.process_order_event("s1", o);
    EXPECT_EQ(ex.active_order_ids().count("OID-2"), 0U);
}
 
TEST(ExecutionEngineTracking, CancelOrderRemovesTrackingEvenWithoutImpl) {
    core::ExecutionEngine ex;
    ex.register_active_order("s1", "OID-1");
 
    utilities::CancelRequest req;
    req.orderid = "OID-1";
    req.symbol = "SYM";
    ex.cancel_order(req);
 
    EXPECT_EQ(ex.active_order_ids().count("OID-1"), 0U);
    EXPECT_TRUE(ex.get_strategy_name_for_order("OID-1").empty());
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
    EXPECT_EQ(ex.active_order_ids().count("OID-1"), 0U);
    EXPECT_EQ(ex.active_order_ids().count("OID-2"), 0U);
    EXPECT_EQ(ex.active_order_ids().count("OID-3"), 1U);
}
 
} // namespace

