/**
 * The pre-trade check (RiskEngine): each limit refuses what passes it and lets the rest through; a
 * strategy's working orders count toward what it would hold; halted, only an order that closes
 * goes; options settled in shares and expiring today are only closed in the last minutes before the
 * close; a limit set out of its range is refused whole.
 */
#include "core/engines/execution.hpp"
#include "core/engines/position.hpp"
#include "core/engines/risk.hpp"
#include "model/portfolio_structure.hpp"
#include "model/symbol.hpp"
#include "model/utility.hpp"
#include <chrono>
#include <gtest/gtest.h>
#include <optional>
#include <stdexcept>
#include <string>

namespace {

using namespace std::chrono;
using model::Direction;

const year_month_day kExpiry = 2099y / December / 18;

auto contract(const std::string& symbol, std::optional<model::OptionType> type = std::nullopt,
              double strike = 0.0) -> model::ContractData {
    model::ContractData c;
    c.symbol = symbol;
    c.trading_class = "SPY";
    if (!type) {
        c.product = model::Product::EQUITY;
        return c;
    }
    c.product = model::Product::OPTION;
    c.size = 100;
    c.option_strike = strike;
    c.option_type = *type;
    c.option_expiry = model::option_expiry_time(kExpiry);
    c.option_underlying = "SPY";
    return c;
}

class RiskEngineTest : public ::testing::Test {
  protected:
    void SetUp() override {
        structure_.ensure_portfolio("SPY");
        structure_.process_underlying_for_portfolio("SPY", contract("SPY"));
        structure_.process_option_for_portfolio("SPY",
                                                contract(call_, model::OptionType::CALL, 600));
        structure_.finalize_all_chains();
        model::PortfolioData* spy = structure_.get_portfolio("SPY");
        spy->physical = true;
        model::OptionData& option = spy->options.at(call_);
        option.bid_price = 1.9;
        option.ask_price = 2.1;
        option.mid_price = 2.0;
        positions_.get_create_strategy_holding("s");
    }

    /** An order for the call: + buys, - sells. */
    auto order(double signed_volume, model::OrderType type = model::OrderType::MARKET,
               double price = 0.0) const -> model::OrderRequest {
        model::OrderRequest req;
        req.symbol = call_;
        req.direction = signed_volume > 0 ? Direction::LONG : Direction::SHORT;
        req.volume = std::abs(signed_volume);
        req.type = type;
        req.price = price;
        return req;
    }

    /** The strategy holds `quantity` of the call. */
    void hold(int quantity) {
        model::TradeData t;
        t.tradeid = "T" + std::to_string(++fills_);
        t.orderid = "O" + std::to_string(fills_);
        t.symbol = call_;
        t.direction = quantity > 0 ? Direction::LONG : Direction::SHORT;
        t.price = 2.0;
        t.volume = std::abs(quantity);
        positions_.process_trade_event("s", t, nullptr);
    }

    auto refusal(const model::OrderRequest& req, model::DateTime now = {})
        -> std::optional<std::string> {
        return risk_.refusal("s", "SPY", req, now);
    }

    const std::string call_ = model::option_symbol(
        {.root = "SPY", .expiry = kExpiry, .type = model::OptionType::CALL, .strike = 600});
    model::PortfolioStructure structure_;
    core::PositionEngine positions_;
    core::ExecutionEngine execution_;
    core::RiskEngine risk_{nullptr, structure_, positions_, execution_};
    int fills_ = 0;
};

TEST_F(RiskEngineTest, TheDefaultsLetAnOrdinaryOrderThrough) {
    EXPECT_EQ(refusal(order(10)), std::nullopt);
    EXPECT_EQ(refusal(order(-10)), std::nullopt); // short options allowed by default
}

TEST_F(RiskEngineTest, AnOrderOverItsSizeIsRefused) {
    risk_.set({{"max_option_order", 5}});
    EXPECT_EQ(refusal(order(5)), std::nullopt);
    ASSERT_TRUE(refusal(order(6)));
    EXPECT_NE(refusal(order(6))->find("max_option_order"), std::string::npos);
}

TEST_F(RiskEngineTest, WorkingOrdersCountTowardWhatTheStrategyWouldHold) {
    risk_.set({{"max_option_position", 6}});
    hold(3);
    model::OrderData working = order(2).create_order_data("W1");
    working.status = model::Status::NOTTRADED;
    execution_.register_active_order("s", "W1");
    execution_.process_order_event("s", working);
    EXPECT_TRUE(refusal(order(2)));          // 3 + 2 working + 2 = 7
    EXPECT_EQ(refusal(order(1)), std::nullopt); // 6
    EXPECT_EQ(refusal(order(-5)), std::nullopt); // toward zero: always
}

TEST_F(RiskEngineTest, NoShortOptionsMeansSellingOnlyWhatIsHeld) {
    risk_.set({{"short_options", 0}});
    hold(1);
    EXPECT_EQ(refusal(order(-1)), std::nullopt);
    ASSERT_TRUE(refusal(order(-2)));
    EXPECT_NE(refusal(order(-2))->find("short"), std::string::npos);
}

TEST_F(RiskEngineTest, APremiumOverTheBudgetIsRefused) {
    risk_.set({{"max_order_premium", 500}});
    EXPECT_EQ(refusal(order(2)), std::nullopt);  // 2 x 2.10 x 100 = 420
    EXPECT_TRUE(refusal(order(3)));              // 630
    EXPECT_EQ(refusal(order(-3)), std::nullopt); // selling pays nothing
    EXPECT_TRUE(refusal(order(3, model::OrderType::LIMIT, 2.0))); // 600 at its limit
}

TEST_F(RiskEngineTest, ALimitFarFromTheMidIsRefused) {
    risk_.set({{"price_band", 0.2}, {"price_band_floor", 0.05}});
    EXPECT_EQ(refusal(order(1, model::OrderType::LIMIT, 2.3)), std::nullopt); // 0.3 <= 0.4
    EXPECT_TRUE(refusal(order(1, model::OrderType::LIMIT, 2.5)));             // 0.5 > 0.4
}

TEST_F(RiskEngineTest, AMarketOrderNeedsATightTwoSidedQuote) {
    risk_.set({{"market_max_spread", 0.15}});
    EXPECT_EQ(refusal(order(1)), std::nullopt); // 0.20 wide on 2.00: 10%
    model::OptionData& option = structure_.get_portfolio("SPY")->options.at(call_);
    option.ask_price = 2.5;
    EXPECT_TRUE(refusal(order(1)));
    option.ask_price = 2.1;
    option.bid_price = 0.0;
    EXPECT_TRUE(refusal(order(1)));
}

TEST_F(RiskEngineTest, HaltedOnlyAnOrderThatClosesGoes) {
    hold(2);
    risk_.halt("a test", model::DateTime{});
    ASSERT_TRUE(refusal(order(1)));
    EXPECT_NE(refusal(order(1))->find("a test"), std::string::npos);
    EXPECT_EQ(refusal(order(-2)), std::nullopt);
    EXPECT_TRUE(refusal(order(-3))); // past flat: it would open a short
    risk_.clear_halt();
    EXPECT_EQ(refusal(order(1)), std::nullopt);
}

TEST_F(RiskEngineTest, OptionsSettledInSharesAreOnlyClosedJustBeforeTheirClose) {
    risk_.set({{"flatten_minutes", 5}});
    hold(1);
    const model::DateTime close = model::option_expiry_time(kExpiry);
    EXPECT_EQ(refusal(order(1), close - minutes(10)), std::nullopt);
    EXPECT_TRUE(refusal(order(1), close - minutes(3)));
    EXPECT_EQ(refusal(order(-1), close - minutes(3)), std::nullopt);
    const model::PortfolioData* spy = structure_.get_portfolio("SPY");
    EXPECT_TRUE(risk_.in_flatten_window(call_, spy, close - minutes(3)));
    EXPECT_FALSE(risk_.in_flatten_window(call_, spy, close - minutes(6)));
}

TEST_F(RiskEngineTest, ALimitOutOfItsRangeIsRefusedWhole) {
    EXPECT_THROW(risk_.set({{"max_option_order", 0}}), std::invalid_argument);
    EXPECT_THROW(risk_.set({{"max_option_order", 2.5}}), std::invalid_argument); // whole numbers
    EXPECT_THROW(risk_.set({{"no_such_limit", 1}}), std::invalid_argument);
    EXPECT_THROW(risk_.set({{"max_option_order", 5}, {"short_options", 3}}), std::invalid_argument);
    EXPECT_EQ(risk_.setting("max_option_order"), 100); // none set
}

TEST_F(RiskEngineTest, ARefusalIsCountedAndKept) {
    risk_.set({{"max_option_order", 1}});
    EXPECT_FALSE(risk_.allow("s", "SPY", order(2), model::DateTime{}));
    EXPECT_TRUE(risk_.allow("s", "SPY", order(1), model::DateTime{}));
    EXPECT_EQ(risk_.refused(), 1U);
    ASSERT_EQ(risk_.recent_refusals().size(), 1U);
    EXPECT_NE(risk_.recent_refusals().front().find(call_), std::string::npos);
}

} // namespace
