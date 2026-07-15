#include "runtime/live/engine_event.hpp"
#include "runtime/main_engine_base.hpp"
#include "utilities/intent.hpp"
#include <gtest/gtest.h>
#include <optional>
#include <string>

namespace {

struct FakeMainForIntent final : runtime_common::MainEngineBase {
    std::optional<std::string> last_cancel_orderid;
    std::optional<std::string> last_log_msg;

    std::string send_order(const std::string& strategy_name,
                           const utilities::OrderRequest& req) override {
        (void)req;
        return "OID-" + strategy_name;
    }
    void cancel_order(const utilities::CancelRequest& req) override {
        last_cancel_orderid = req.orderid;
    }
    void put_log_intent(const utilities::LogData& log) override { last_log_msg = log.msg; }

    // Minimal pure-virtual implementations not used in these tests
    void put_event(const utilities::Event&) override {}
    void put_event(utilities::Event&&) override {}
    utilities::PortfolioSnapshot* acquire_snapshot() override { return nullptr; }
    utilities::OrderData* acquire_order() override { return nullptr; }
    utilities::TradeData* acquire_trade() override { return nullptr; }
    std::string send_order_to_gateway(const utilities::OrderRequest& req) override {
        (void)req;
        return {};
    }
};

TEST(IntentRouting, SendOrderReturnsOrderId) {
    FakeMainForIntent main;
    engines::EventEngine ee(&main);

    utilities::OrderRequest req;
    req.symbol = "SPXW-TEST";
    const auto out = ee.put_intent(utilities::IntentSendOrder{"stratA", req});
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(*out, "OID-stratA");
}

TEST(IntentRouting, CancelOrderCallsHandler) {
    FakeMainForIntent main;
    engines::EventEngine ee(&main);

    utilities::CancelRequest c;
    c.orderid = "OID-123";
    auto out = ee.put_intent(utilities::IntentCancelOrder{c});
    EXPECT_FALSE(out.has_value());
    ASSERT_TRUE(main.last_cancel_orderid.has_value());
    EXPECT_EQ(*main.last_cancel_orderid, "OID-123");
}

TEST(IntentRouting, LogCallsHandler) {
    FakeMainForIntent main;
    engines::EventEngine ee(&main);

    utilities::LogData log;
    log.msg = "hello";
    auto out = ee.put_intent(utilities::IntentLog{log});
    EXPECT_FALSE(out.has_value());
    ASSERT_TRUE(main.last_log_msg.has_value());
    EXPECT_EQ(*main.last_log_msg, "hello");
}

} // namespace
