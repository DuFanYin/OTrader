#include "adapters/gateway/backtest_gateway.hpp"
#include "tests/lone_backtest.hpp"
#include <cstdint>
#include <gtest/gtest.h>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using model::Side;
using model::Leg;
using model::OrderRequest;

// Two calls, one bought and one sold, as a combo order of the given type and limit.
OrderRequest call_spread(const char* bought_symbol, const char* sold_symbol,
                         model::OrderType type = model::OrderType::MARKET,
                         double limit = 0.0) {
    Leg bought;
    bought.symbol = bought_symbol;
    bought.side = Side::BUY;
    bought.ratio = 1;
    Leg sold;
    sold.symbol = sold_symbol;
    sold.side = Side::SELL;
    sold.ratio = 1;
    OrderRequest req;
    req.symbol = "SPREAD";
    req.type = type;
    req.price = limit;
    req.quantity = 1;
    req.legs = std::vector<Leg>{bought, sold};
    return req;
}

// A bought call and a sold far wing, as in the wings of an iron fly late on expiry day.
OrderRequest call_spread() { return call_spread("SPY-20991218-C-601", "SPY-20991218-C-603"); }

/** The option quoted in SPY's frame, listed first if it is not. */
void quote_option(model::Portfolios& structure, const std::string& symbol, double bid,
                  double ask, double bid_size = 0.0, double ask_size = 0.0) {
    model::Portfolio& spy = *structure.find_portfolio("SPY");
    if (!spy.option(symbol)) {
        model::Contract c;
        c.symbol = symbol;
        c.multiplier = 100;
        structure.load_contracts(std::vector{c});
    }
    model::PortfolioSnapshot s = spy.snapshot();
    s.assign(spy.frame());
    s.priced = true;
    const std::uint32_t column = *spy.listing().column(symbol);
    s.values(&model::Frame::bid)[column] = bid;
    s.values(&model::Frame::ask)[column] = ask;
    s.values(&model::Frame::bid_size)[column] = bid_size;
    s.values(&model::Frame::ask_size)[column] = ask_size;
    spy.apply(s);
}

void quote(core::Main& main, const std::string& symbol, double bid, double ask) {
    quote_option(*main.portfolios(), symbol, bid, ask);
}

class BacktestComboFill : public ::testing::Test {
  protected:
    void SetUp() override {
        main.portfolios()->ensure_portfolio("SPY");
        ASSERT_NE(main.portfolios()->find_portfolio("SPY"), nullptr);
        gateway().configure_execution({});
    }

    adapters::BacktestGateway& gateway() { return *backtest.backtest_gateway(); }
    /** Send the order for s1 and run the next bar: true when it filled. */
    bool fill(const OrderRequest& req) {
        const std::string order_id = gateway().send_order(req);
        main.bookkeeping_engine()->add_order("s1", req.create_order(order_id)); // as Main does
        gateway().execute_pending_orders();
        const model::Order* order = main.bookkeeping_engine()->find_order(order_id);
        return order != nullptr && order->status == model::Status::FILLED;
    }

    test::LoneBacktest backtest;
    core::Main& main = backtest.main();
};

TEST_F(BacktestComboFill, EachLegTradesOnItsOwnSideAndAZeroBidSellsForNothing) {
    quote(main, "SPY-20991218-C-601", 0.30, 0.32);
    quote(main, "SPY-20991218-C-603", 0.00, 0.02);
    ASSERT_TRUE(fill(call_spread()));

    auto* bought = main.bookkeeping_engine()->find_fill("backtest_trade_1_leg_0");
    auto* sold = main.bookkeeping_engine()->find_fill("backtest_trade_1_leg_1");
    ASSERT_NE(bought, nullptr);
    ASSERT_NE(sold, nullptr);
    EXPECT_DOUBLE_EQ(bought->price, 0.32);
    EXPECT_DOUBLE_EQ(sold->price, 0.0); // not the combo's aggregate price
    // A combo fills as its legs only.
    EXPECT_EQ(main.bookkeeping_engine()->find_fill("backtest_trade_1"), nullptr);
}

TEST_F(BacktestComboFill, ABoughtLegWithNoOfferDoesNotFill) {
    quote(main, "SPY-20991218-C-601", 0.30, 0.00);
    quote(main, "SPY-20991218-C-603", 0.01, 0.02);
    EXPECT_FALSE(fill(call_spread()));
    EXPECT_EQ(main.bookkeeping_engine()->find_fill("backtest_trade_1"), nullptr);
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
    EXPECT_NEAR(main.bookkeeping_engine()->find_fill("backtest_trade_1_leg_0")->price -
                    main.bookkeeping_engine()->find_fill("backtest_trade_1_leg_1")->price,
                -0.28, 1e-12);
}

OrderRequest one_call(const char* symbol, Side side) {
    Leg leg;
    leg.symbol = symbol;
    leg.side = side;
    leg.ratio = 1;
    OrderRequest req;
    req.symbol = symbol;
    req.type = model::OrderType::MARKET;
    req.quantity = 1;
    req.legs = std::vector<Leg>{leg};
    return req;
}

TEST_F(BacktestComboFill, AMultiLegOrderFillsAtItsFillPositionInEachLegsSpread) {
    gateway().configure_execution({.multi_leg_fill_position = 0.25});
    quote(main, "SPY-20991218-C-601", 0.30, 0.32);
    quote(main, "SPY-20991218-C-603", 0.10, 0.12);
    ASSERT_TRUE(fill(call_spread()));
    core::BookkeepingEngine& orders = *main.bookkeeping_engine();
    EXPECT_NEAR(orders.find_fill("backtest_trade_1_leg_0")->price, 0.31 + 0.25 * 0.01, 1e-12);
    EXPECT_NEAR(orders.find_fill("backtest_trade_1_leg_1")->price, 0.11 - 0.25 * 0.01, 1e-12);
}

TEST_F(BacktestComboFill, AnOrderOfOneLegHasItsOwnFillPosition) {
    gateway().configure_execution(
        {.fill_position = 0.5, .multi_leg_fill_position = 0.25});
    quote(main, "SPY-20991218-C-601", 0.30, 0.32);
    ASSERT_TRUE(fill(one_call("SPY-20991218-C-601", Side::BUY)));
    EXPECT_NEAR(main.bookkeeping_engine()->find_fill("backtest_trade_1")->price, 0.315, 1e-12);
}

TEST_F(BacktestComboFill, AFillPositionAboveOneFillsBeyondTheQuoteButASaleNeverBelowNothing) {
    gateway().configure_execution({.multi_leg_fill_position = 2.0});
    quote(main, "SPY-20991218-C-601", 0.30, 0.32);
    quote(main, "SPY-20991218-C-603", 0.00, 0.02);
    ASSERT_TRUE(fill(call_spread()));
    core::BookkeepingEngine& orders = *main.bookkeeping_engine();
    EXPECT_NEAR(orders.find_fill("backtest_trade_1_leg_0")->price, 0.33, 1e-12);
    EXPECT_DOUBLE_EQ(orders.find_fill("backtest_trade_1_leg_1")->price, 0.0);
}

TEST_F(BacktestComboFill, ACrossedQuoteIsNoQuote) {
    quote(main, "SPY-20991218-C-601", 0.33, 0.32);
    quote(main, "SPY-20991218-C-603", 0.10, 0.12);
    EXPECT_FALSE(fill(call_spread()));
    EXPECT_FALSE(fill(one_call("SPY-20991218-C-601", Side::BUY)));
}

TEST_F(BacktestComboFill, ANegativeFillPositionIsRefused) {
    EXPECT_THROW(gateway().configure_execution({.fill_position = -0.1}), std::invalid_argument);
    EXPECT_THROW(gateway().configure_execution({.multi_leg_fill_position = -0.1}),
                 std::invalid_argument);
}

} // namespace
