/**
 * The backtest's fill models: Quote fills an order whole at the quote; ShownSize fills at most the
 * size shown at the order's side each bar, what the bar's earlier fills took counted, the rest
 * waiting for the bars after (a combo: as much as its thinnest leg allows). delay_bars holds an
 * order that many bars more before it can fill. Fees are charged on what fills.
 */
#include "adapters/gateway/backtest_gateway.hpp"
#include "tests/lone_backtest.hpp"
#include <cstdint>
#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace {

using adapters::BacktestGateway;
using model::Side;
using model::OrderRequest;

constexpr const char* kCall = "SPY-20991218-C-601";
constexpr const char* kWing = "SPY-20991218-C-603";

OrderRequest buy(const char* symbol, double quantity) {
    OrderRequest req;
    req.symbol = symbol;
    req.type = model::OrderType::MARKET;
    req.quantity = quantity;
    req.legs = {{.ratio = 1, .side = Side::BUY, .symbol = symbol}};
    return req;
}

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

class BacktestFillModel : public ::testing::Test {
  protected:
    void SetUp() override {
        main.portfolios()->ensure_portfolio("SPY");
        ASSERT_NE(main.portfolios()->find_portfolio("SPY"), nullptr);
        configure(BacktestGateway::FillModel::ShownSize);
    }

    void configure(BacktestGateway::FillModel fill, int delay_bars = 0, double fee_scale = 0.0) {
        gateway().configure_execution(
            {.fee_scale = fee_scale, .fill = fill, .delay_bars = delay_bars});
    }
    /** The option quoted at 1.00 / 1.10 with these sizes shown. */
    void quote(const std::string& symbol, double bid_size, double ask_size) {
        quote_option(*main.portfolios(), symbol, 1.00, 1.10, bid_size, ask_size);
    }
    BacktestGateway& gateway() { return *backtest.backtest_gateway(); }
    std::string send(const OrderRequest& req) {
        const std::string order_id = gateway().send_order(req);
        main.bookkeeping_engine()->add_order("s1", req.create_order(order_id)); // as Main does
        return order_id;
    }
    /** The next bar: what the quotes allow of the orders waiting fills. */
    void bar() { gateway().execute_pending_orders(); }
    double filled(const std::string& order_id) {
        const model::Order* order = main.bookkeeping_engine()->find_order(order_id);
        return order != nullptr ? order->filled : 0.0;
    }
    model::Status status(const std::string& order_id) {
        const model::Order* order = main.bookkeeping_engine()->find_order(order_id);
        return order != nullptr ? order->status : model::Status::SUBMITTING;
    }

    test::LoneBacktest backtest;
    core::Main& main = backtest.main();
};

TEST_F(BacktestFillModel, QuoteFillsTheWholeOrderWhateverSizeIsShown) {
    configure(BacktestGateway::FillModel::Quote);
    quote(kCall, 0, 4);
    const std::string id = send(buy(kCall, 10));
    bar();
    EXPECT_EQ(status(id), model::Status::FILLED);
    EXPECT_DOUBLE_EQ(filled(id), 10);
}

TEST_F(BacktestFillModel, ShownSizeFillsWhatIsShownEachBarAndTheRestWaits) {
    configure(BacktestGateway::FillModel::ShownSize, 0, /*fee_scale=*/1.0);
    quote(kCall, 0, 4);
    const std::string id = send(buy(kCall, 10));
    bar();
    EXPECT_EQ(status(id), model::Status::PARTLY_FILLED);
    EXPECT_DOUBLE_EQ(filled(id), 4);
    EXPECT_DOUBLE_EQ(gateway().cumulative_fees(), 4 * 0.65); // on what filled
    quote(kCall, 0, 0); // nothing shown: nothing fills, the order waits
    bar();
    EXPECT_DOUBLE_EQ(filled(id), 4);
    quote(kCall, 0, 4);
    bar();
    bar();
    EXPECT_EQ(status(id), model::Status::FILLED);
    EXPECT_DOUBLE_EQ(filled(id), 10);
    EXPECT_DOUBLE_EQ(gateway().cumulative_fees(), 10 * 0.65);
    EXPECT_DOUBLE_EQ(main.bookkeeping_engine()->find_fill("backtest_trade_3")->quantity, 2);
}

TEST_F(BacktestFillModel, FeesByPriceAnIndexOptionsExchangeFeeAndAnOrdersLeastOverItsFills) {
    configure(BacktestGateway::FillModel::ShownSize, 0, /*fee_scale=*/1.0);
    quote(kCall, 0, 1);
    const std::string id = send(buy(kCall, 3)); // at the ask, 1.10: IBKR's 0.65 a contract
    bar();
    EXPECT_DOUBLE_EQ(filled(id), 1);
    EXPECT_DOUBLE_EQ(gateway().cumulative_fees(), 1.0); // 0.65: the order's least
    quote(kCall, 0, 2);
    bar();
    EXPECT_EQ(status(id), model::Status::FILLED);
    EXPECT_NEAR(gateway().cumulative_fees(), 3 * 0.65, 1e-12); // the least once, not each fill
    EXPECT_DOUBLE_EQ(model::tier_fee(BacktestGateway::kCommission, 0.07), 0.50);
    EXPECT_DOUBLE_EQ(model::tier_fee(BacktestGateway::kCommission, 0.04), 0.25);
    const model::PortfolioSpec* spxw = model::find_portfolio("SPXW");
    ASSERT_NE(spxw, nullptr);
    EXPECT_DOUBLE_EQ(model::tier_fee(spxw->exchange_fee, 0.80), 0.50); // Cboe's index fee
    EXPECT_DOUBLE_EQ(model::tier_fee(spxw->exchange_fee, 3.00), 0.59);
    EXPECT_DOUBLE_EQ(model::tier_fee(model::find_portfolio("SPY")->exchange_fee, 3.00), 0.0);
    EXPECT_THROW(gateway().configure_execution({.fee_scale = -1.0}), std::invalid_argument);
}

TEST_F(BacktestFillModel, OrdersInOneBarShareTheSizeShownInTheOrderSent) {
    quote(kCall, 0, 5);
    const std::string first = send(buy(kCall, 3));
    const std::string second = send(buy(kCall, 3));
    bar();
    EXPECT_DOUBLE_EQ(filled(first), 3);
    EXPECT_DOUBLE_EQ(filled(second), 2);
    bar(); // a new bar shows its size again
    EXPECT_EQ(status(second), model::Status::FILLED);
}

TEST_F(BacktestFillModel, AComboFillsAsMuchAsItsThinnestLegAllows) {
    quote(kCall, 0, 6);
    quote(kWing, 3, 0); // sold at the bid: 3 shown, two a combo
    model::Leg bought{.ratio = 1, .side = Side::BUY, .symbol = kCall};
    model::Leg sold{.ratio = 2, .side = Side::SELL, .symbol = kWing};
    OrderRequest req = buy("SPREAD", 5);
    req.legs = {bought, sold};
    const std::string id = send(req);
    bar();
    EXPECT_DOUBLE_EQ(filled(id), 1); // floor(3 / 2)
    EXPECT_DOUBLE_EQ(main.bookkeeping_engine()->find_fill("backtest_trade_1_leg_1")->quantity, 2);
}

TEST_F(BacktestFillModel, ADelayHoldsAnOrderThatManyBarsMore) {
    configure(BacktestGateway::FillModel::Quote, /*delay_bars=*/2);
    quote(kCall, 0, 4);
    const std::string id = send(buy(kCall, 1));
    bar();
    bar();
    EXPECT_DOUBLE_EQ(filled(id), 0);
    bar();
    EXPECT_EQ(status(id), model::Status::FILLED);
}

} // namespace
