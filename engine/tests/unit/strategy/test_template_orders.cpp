/**
 * The template's orders, through the backtest's venue: one kind, units of legs (one leg its
 * contract's own order); a limit is the net a unit; whatever traded them, options are held by
 * contract, and those opened in one combo close in one order; closing everything leaves the
 * holding flat.
 */
#include "tests/lone_backtest.hpp"
#include "model/symbol.hpp"
#include "strategy/strategy_registry.hpp"
#include "strategy/strategy.hpp"
#include <array>
#include <chrono>
#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace {

using namespace std::chrono;
using model::ComboType;
using model::Side;

class TemplateOrderProbe final : public strategy::Strategy {
  public:
    using Strategy::Strategy;

    void on_timer() override {}
};

REGISTER_STRATEGY(TemplateOrderProbe);

auto option(model::OptionType type) -> std::string {
    return model::option_symbol(
        {.root = "SPY", .expiry = 2099y / December / 18, .type = type, .strike = 600});
}

class TemplateOrders : public ::testing::Test {
  protected:
    void SetUp() override {
        model::Portfolios& s = *main.portfolios();
        s.ensure_portfolios_created();
        model::Contract spy;
        spy.symbol = "SPY";
        spy.product = model::Product::EQUITY;
        std::vector<model::Contract> contracts{spy};
        for (const std::string& symbol : {call_, put_}) {
            model::Contract c;
            c.symbol = symbol;
            c.product = model::Product::OPTION;
            c.multiplier = 100;
            contracts.push_back(c);
        }
        s.load_contracts(contracts);
        model::Portfolio& p = *s.find_portfolio("SPY");
        model::PortfolioSnapshot quotes = p.snapshot();
        quotes.underlying_bid = 599.99;
        quotes.underlying_ask = 600.01;
        for (const std::string& symbol : {call_, put_}) {
            quotes.values(&model::Frame::bid)[*p.listing().column(symbol)] = 1.9;
            quotes.values(&model::Frame::ask)[*p.listing().column(symbol)] = 2.1;
        }
        quotes.priced = true;
        p.apply(quotes);
        backtest.configure_execution({});
        name_ = main.add_strategy("TemplateOrderProbe", "SPY", {});
        main.init_strategy(name_);
        main.start_strategy(name_);
        probe_ = main.strategy_engine()->find_strategy(name_);
    }

    /** The next bar: what was sent fills at its quotes. */
    void fill() { backtest.backtest_gateway()->execute_pending_orders(); }
    auto sent(const std::string& order_id) -> const model::Order& {
        const model::Order* order = main.bookkeeping_engine()->find_order(order_id);
        EXPECT_NE(order, nullptr) << order_id;
        return *order;
    }
    auto active() -> std::vector<model::Order> {
        std::vector<model::Order> out;
        main.bookkeeping_engine()->for_each_active_order(
            {}, [&](const model::Order& o) { out.push_back(o); });
        return out;
    }
    /** A straddle at 600, bought or sold. */
    auto straddle(Side side) -> std::vector<model::Leg> {
        return probe_->combo_legs(ComboType::STRADDLE,
                                  {{"call", call_}, {"put", put_}}, side);
    }
    /** Held or closed. */
    auto held(const std::string& key) -> const model::OptionPosition& {
        const model::StrategyHolding& h = *probe_->holding();
        const auto it = h.options.find(key);
        return it != h.options.end() ? it->second : h.closed_options.at(key);
    }

    const std::string call_ = option(model::OptionType::CALL);
    const std::string put_ = option(model::OptionType::PUT);
    test::LoneBacktest backtest;
    core::Main& main = backtest.main();
    std::string name_;
    strategy::Strategy* probe_ = nullptr;
};

TEST_F(TemplateOrders, OneLegIsItsContractsOwnOrderAndHeldAsItself) {
    const std::string id =
        probe_->send_order(std::array{probe_->leg(call_, Side::BUY)}, 0.0, 2);
    ASSERT_FALSE(id.empty());
    EXPECT_FALSE(sent(id).is_combo());
    EXPECT_EQ(sent(id).symbol, call_);
    EXPECT_EQ(sent(id).reference, "Strategy_" + name_); // tagged as the strategy's
    fill();
    EXPECT_EQ(held(call_).quantity, 2);
    EXPECT_DOUBLE_EQ(held(call_).avg_cost, 2.1);

    const std::string sold = probe_->send_order(std::array{probe_->leg(put_, Side::SELL)});
    EXPECT_FALSE(sent(sold).is_combo());
    EXPECT_EQ(sent(sold).legs.front().side, Side::SELL);
    EXPECT_TRUE(probe_->send_order({}).empty());
    EXPECT_TRUE(probe_->send_order(std::array{probe_->leg("SPY-20991218-C-999", Side::BUY)})
                    .empty()); // not listed
}

TEST_F(TemplateOrders, AStraddleIsOneOrderHeldByContractAndClosedAsOne) {
    const std::string id = probe_->send_order(straddle(Side::BUY), 0.0, 1,
                                              model::OrderType::MARKET, ComboType::STRADDLE);
    ASSERT_FALSE(id.empty());
    EXPECT_TRUE(sent(id).is_combo());
    EXPECT_EQ(sent(id).legs.size(), 2U);
    fill();
    for (const std::string& symbol : {call_, put_}) {
        EXPECT_EQ(held(symbol).quantity, 1) << symbol;
        EXPECT_EQ(held(symbol).combo, call_ + "|" + put_);
        EXPECT_EQ(held(symbol).combo_type, ComboType::STRADDLE);
    }

    probe_->close_all_strategy_positions();
    ASSERT_EQ(active().size(), 1U); // one order for both
    fill();
    for (const std::string& symbol : {call_, put_}) {
        EXPECT_EQ(held(symbol).quantity, 0) << symbol;
        EXPECT_NEAR(held(symbol).realized_pnl, (1.9 - 2.1) * 100.0, 1e-9); // bought 2.1, sold 1.9
    }
}

TEST_F(TemplateOrders, AContractIsOnePositionWhicheverOrdersTradedIt) {
    probe_->send_order(straddle(Side::BUY), 0.0, 1, model::OrderType::MARKET,
                       ComboType::STRADDLE);
    fill();
    probe_->send_order(std::array{probe_->leg(call_, Side::SELL)});
    fill();
    EXPECT_EQ(probe_->holding()->options.size(), 1U);
    EXPECT_EQ(probe_->holding()->closed_options.size(), 1U);
    EXPECT_EQ(held(call_).quantity, 0);
    EXPECT_EQ(held(put_).quantity, 1);

    probe_->close_all_strategy_positions(); // the put alone is left of the straddle: its own order
    const std::vector<model::Order> closing = active();
    ASSERT_EQ(closing.size(), 1U);
    EXPECT_FALSE(closing.front().is_combo());
    EXPECT_EQ(closing.front().symbol, put_);
    fill();
    EXPECT_TRUE(probe_->holding()->is_flat());
}

TEST_F(TemplateOrders, LegsCarryTheirSidesAndRatios) {
    const std::vector legs{probe_->leg(call_, Side::BUY),
                           probe_->leg(put_, Side::SELL, 2)};
    const std::string id = probe_->send_order(legs, 0.0, 2);
    ASSERT_FALSE(id.empty());
    EXPECT_TRUE(sent(id).is_combo());
    EXPECT_EQ(sent(id).combo_type, ComboType::CUSTOM);
    fill();
    EXPECT_EQ(held(call_).quantity, 2);
    EXPECT_EQ(held(put_).quantity, -4);
    EXPECT_EQ(held(put_).combo_type, ComboType::CUSTOM);

    // Closed in one order of the smallest whole units: 2 of (sell 1 call, buy 2 puts).
    probe_->close_options(std::vector{held(call_), held(put_)});
    const std::vector<model::Order> closing = active();
    ASSERT_EQ(closing.size(), 1U);
    EXPECT_EQ(closing.front().quantity, 2);
    ASSERT_EQ(closing.front().legs.size(), 2U);
    for (const model::Leg& leg : closing.front().legs) {
        EXPECT_EQ(leg.side, leg.symbol == call_ ? Side::SELL : Side::BUY);
        EXPECT_EQ(leg.ratio, leg.symbol == call_ ? 1 : 2);
    }
    fill();
    EXPECT_TRUE(probe_->holding()->is_flat());
}

TEST_F(TemplateOrders, ALimitIsTheNetAUnitPaidOrReceived) {
    // A straddle sold for at least 3.6 a unit: quoted 1.9 bid each, 3.8 received.
    probe_->send_order(straddle(Side::SELL), -3.6, 1, model::OrderType::LIMIT,
                       ComboType::STRADDLE);
    fill();
    EXPECT_EQ(held(call_).quantity, -1);
    // A call bought for at most 2.0: the ask is 2.1, nothing yet.
    probe_->send_order(std::array{probe_->leg(call_, Side::BUY)}, 2.0, 1,
                       model::OrderType::LIMIT);
    fill();
    EXPECT_EQ(held(call_).quantity, -1);
    EXPECT_TRUE(probe_->has_active_order());
}

TEST_F(TemplateOrders, ClosingEverythingLeavesTheHoldingFlat) {
    probe_->send_order(std::array{probe_->leg(call_, Side::SELL)}, 0.0, 3);
    probe_->send_order(straddle(Side::BUY), 0.0, 2, model::OrderType::MARKET,
                       ComboType::STRADDLE);
    probe_->send_order(std::array{probe_->leg("SPY", Side::BUY)}, 0.0, 10);
    fill();
    ASSERT_FALSE(probe_->holding()->is_flat());
    EXPECT_EQ(probe_->holding()->shares.quantity, 10);

    probe_->close_all_strategy_positions();
    fill();
    EXPECT_TRUE(probe_->holding()->is_flat());
    EXPECT_FALSE(probe_->holds_options());
}

TEST_F(TemplateOrders, AComboThatCannotBeBuiltIsNotSent) {
    const std::size_t before = main.bookkeeping_engine()->order_count();
    EXPECT_TRUE(probe_
                    ->send_order(probe_->combo_legs(ComboType::STRADDLE, {{"call", call_}},
                                                    Side::BUY))
                    .empty()); // no put
    EXPECT_EQ(main.bookkeeping_engine()->order_count(), before);
    EXPECT_FALSE(probe_->has_active_order());
}

} // namespace
