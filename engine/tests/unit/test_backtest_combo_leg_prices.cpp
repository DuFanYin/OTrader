#include "adapters/backtest/engine_gateway_backtest.hpp"
#include "core/engine_backtest.hpp"
#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace {

using model::Direction;
using model::Leg;
using model::OrderRequest;

// Two calls, one bought and one sold, as a combo order of the given type and limit.
OrderRequest call_spread(const char* bought_symbol, const char* sold_symbol,
                         model::OrderType type = model::OrderType::MARKET,
                         double limit = 0.0) {
    Leg bought;
    bought.symbol = bought_symbol;
    bought.direction = Direction::LONG;
    bought.ratio = 1;
    Leg sold;
    sold.symbol = sold_symbol;
    sold.direction = Direction::SHORT;
    sold.ratio = 1;
    OrderRequest req;
    req.symbol = "SPREAD";
    req.direction = Direction::LONG;
    req.type = type;
    req.price = limit;
    req.volume = 1;
    req.is_combo = true;
    req.legs = std::vector<Leg>{bought, sold};
    return req;
}

// A bought call and a sold far wing, as in the wings of an iron fly late on expiry day.
OrderRequest call_spread() { return call_spread("SPY-20991218-C-601", "SPY-20991218-C-603"); }

void quote(core::backtest::MainEngine& main, const std::string& symbol, double bid, double ask) {
    auto& option = main.get_portfolio("SPY")->options[symbol];
    option.bid_price = bid;
    option.ask_price = ask;
}

class BacktestComboFill : public ::testing::Test {
  protected:
    void SetUp() override {
        main.portfolio_structure()->ensure_portfolio("SPY");
        ASSERT_NE(main.get_portfolio("SPY"), nullptr);
        gateway().configure_execution(/*fee_rate=*/0.0, /*slippage_bps=*/0.0);
    }

    adapters::BacktestGateway& gateway() { return *main.backtest_gateway(); }
    /** Send the order for s1 and run the next bar: true when it filled. */
    bool fill(const OrderRequest& req) {
        const std::string orderid = gateway().send_order(req);
        main.execution_engine()->register_active_order("s1", orderid);
        gateway().execute_pending_orders();
        const model::OrderData* order = main.get_order(orderid);
        return order != nullptr && order->status == model::Status::ALLTRADED;
    }

    core::backtest::MainEngine main;
};

TEST_F(BacktestComboFill, EachLegTradesOnItsOwnSideAndAZeroBidSellsForNothing) {
    quote(main, "SPY-20991218-C-601", 0.30, 0.32);
    quote(main, "SPY-20991218-C-603", 0.00, 0.02);
    ASSERT_TRUE(fill(call_spread()));

    auto* bought = main.get_trade("backtest_trade_1_leg_0");
    auto* sold = main.get_trade("backtest_trade_1_leg_1");
    ASSERT_NE(bought, nullptr);
    ASSERT_NE(sold, nullptr);
    EXPECT_DOUBLE_EQ(bought->price, 0.32);
    EXPECT_DOUBLE_EQ(sold->price, 0.0); // not the combo's aggregate price
    EXPECT_EQ(main.get_trade("backtest_trade_1"), nullptr); // a combo fills as its legs only
}

TEST_F(BacktestComboFill, ABoughtLegWithNoOfferDoesNotFill) {
    quote(main, "SPY-20991218-C-601", 0.30, 0.00);
    quote(main, "SPY-20991218-C-603", 0.01, 0.02);
    EXPECT_FALSE(fill(call_spread()));
    EXPECT_EQ(main.get_trade("backtest_trade_1"), nullptr);
}

// Selling a call spread is buying the combo for a credit: the net price is negative and the
// limit caps it, as with the IB gateway's BAG orders.
TEST_F(BacktestComboFill, ACreditLimitCapsTheNetPrice) {
    quote(main, "SPY-20991218-C-601", 0.30, 0.32);
    quote(main, "SPY-20991218-C-603", 0.00, 0.02);
    const auto credit = [](double limit) {
        return call_spread("SPY-20991218-C-603", "SPY-20991218-C-601", model::OrderType::LIMIT, limit);
    };
    EXPECT_FALSE(fill(credit(-0.30)));
    ASSERT_TRUE(fill(credit(-0.25)));
    // the net, from the legs: bought 603 at 0.02, sold 601 at 0.30
    EXPECT_NEAR(main.get_trade("backtest_trade_1_leg_0")->price -
                    main.get_trade("backtest_trade_1_leg_1")->price,
                -0.28, 1e-12);
}

TEST_F(BacktestComboFill, MarketSlippageMovesEveryLegAgainstUs) {
    gateway().configure_execution(/*fee_rate=*/0.0, /*slippage_bps=*/100.0);
    quote(main, "SPY-20991218-C-601", 0.30, 0.32);
    quote(main, "SPY-20991218-C-603", 0.10, 0.12);
    ASSERT_TRUE(fill(call_spread()));
    EXPECT_NEAR(main.get_trade("backtest_trade_1_leg_0")->price, 0.32 * 1.01, 1e-12);
    EXPECT_NEAR(main.get_trade("backtest_trade_1_leg_1")->price, 0.10 * 0.99, 1e-12);
}

} // namespace
