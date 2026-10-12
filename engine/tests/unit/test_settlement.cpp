/**
 * Settlement and the positions check (PositionEngine): expired options close out at their
 * intrinsic value in cash, or at nothing with the shares they deliver; a combo's legs close with
 * it; the holdings summed by contract meet the broker's, each difference marked.
 */
#include "core/engines/position.hpp"
#include "model/portfolio.hpp"
#include "model/portfolio_structure.hpp"
#include "model/symbol.hpp"
#include "model/utility.hpp"
#include <algorithm>
#include <chrono>
#include <gtest/gtest.h>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace std::chrono;
using model::Direction;
using model::OptionType;

constexpr year_month_day kFriday{year{2025}, March, day{14}};
constexpr year_month_day kNextFriday{year{2025}, March, day{21}};

auto option(const std::string& root, year_month_day expiry, OptionType type, double strike)
    -> std::string {
    return model::option_symbol(
        {.root = root, .expiry = expiry, .type = type, .strike = strike});
}

auto single(model::StrategyHolding& h, const std::string& symbol, int quantity) {
    model::OptionPositionData p(symbol);
    p.quantity = quantity;
    h.optionPositions[symbol] = p;
}

auto leg(const std::string& symbol, int quantity) -> model::OptionPositionData {
    model::OptionPositionData p(symbol);
    p.quantity = quantity;
    return p;
}

/** A portfolio as settlement reads it: its underlying, and how its options settle. */
auto portfolio(const std::string& name, const std::string& underlying, bool physical)
    -> model::PortfolioData {
    model::PortfolioData p(name);
    p.underlying_symbol = underlying;
    p.physical = physical;
    return p;
}

using Booked = core::PositionEngine::Settlement;

/** Settle strategy "s" of the engine. */
auto settle(const core::PositionEngine& engine, year_month_day expiry, double price,
            const model::PortfolioData& p) -> std::vector<Booked> {
    return engine.settle_expiry("s", expiry, price, p);
}

/** The settlement whose order is for `symbol`. */
auto for_symbol(const std::vector<Booked>& all, const std::string& symbol) -> const Booked& {
    for (const auto& b : all) {
        if (b.order.symbol == symbol) {
            return b;
        }
    }
    throw std::runtime_error("no settlement for " + symbol);
}

/** An engine with one strategy, "s", holding what each test puts in. */
class Settlement : public ::testing::Test {
  protected:
    void SetUp() override { engine_.get_create_strategy_holding("s"); }
    model::StrategyHolding& holding() { return engine_.get_holding("s"); }

    core::PositionEngine engine_;
};

TEST_F(Settlement, CashClosesAtIntrinsicValue) {
    const std::string call = option("SPXW", kFriday, OptionType::CALL, 5800);
    const std::string put = option("SPXW", kFriday, OptionType::PUT, 5900);
    const std::string otm = option("SPXW", kFriday, OptionType::CALL, 6000);
    single(holding(), call, 2);
    single(holding(), put, -1);
    single(holding(), otm, 3);

    const auto out = settle(engine_, kFriday, 5850.0, portfolio("SPXW", "SPX", false));
    ASSERT_EQ(out.size(), 3U); // no shares change hands
    const auto& c = for_symbol(out, call);
    EXPECT_EQ(c.order.status, model::Status::ALLTRADED);
    EXPECT_EQ(c.order.reference, "Settlement");
    ASSERT_EQ(c.fills.size(), 1U);
    EXPECT_EQ(c.fills[0].direction, Direction::SHORT);
    EXPECT_EQ(c.fills[0].volume, 2.0);
    EXPECT_DOUBLE_EQ(c.fills[0].price, 50.0);
    EXPECT_EQ(c.fills[0].datetime, model::option_expiry_time(kFriday));
    const auto& p = for_symbol(out, put);
    EXPECT_EQ(p.fills[0].direction, Direction::LONG); // a short put bought back
    EXPECT_DOUBLE_EQ(p.fills[0].price, 50.0);
    EXPECT_DOUBLE_EQ(for_symbol(out, otm).fills[0].price, 0.0);
}

TEST_F(Settlement, InSharesTheMoneyOptionsDeliverAtTheirStrike) {
    const std::string long_call = option("SPY", kFriday, OptionType::CALL, 580);
    const std::string short_put = option("SPY", kFriday, OptionType::PUT, 590);
    const std::string otm_call = option("SPY", kFriday, OptionType::CALL, 600);
    const std::string by_a_hair = option("SPY", kFriday, OptionType::PUT, 585.005);
    single(holding(), long_call, 1);
    single(holding(), short_put, -2);
    single(holding(), otm_call, 1);
    single(holding(), by_a_hair, 1); // in the money by half a cent: not exercised

    const auto out = settle(engine_, kFriday, 585.0, portfolio("SPY", "SPY", true));
    for (const std::string& o : {long_call, short_put, otm_call, by_a_hair}) {
        EXPECT_DOUBLE_EQ(for_symbol(out, o).fills.at(0).price, 0.0) << o; // the option is gone
    }
    std::vector<model::TradeData> shares;
    for (const auto& b : out) {
        if (b.order.symbol == "SPY") {
            shares.push_back(b.fills.at(0));
        }
    }
    ASSERT_EQ(shares.size(), 2U);
    std::ranges::sort(shares, {}, &model::TradeData::price);
    EXPECT_EQ(shares[0].direction, Direction::LONG); // the long call: 100 bought at 580
    EXPECT_EQ(shares[0].volume, 100.0);
    EXPECT_DOUBLE_EQ(shares[0].price, 580.0);
    EXPECT_EQ(shares[1].direction, Direction::LONG); // the short put, assigned: 200 bought at 590
    EXPECT_EQ(shares[1].volume, 200.0);
    EXPECT_DOUBLE_EQ(shares[1].price, 590.0);
}

TEST_F(Settlement, OnlyTheExpiryAskedForAndOnlyOncePastTheClose) {
    const std::string friday = option("SPY", kFriday, OptionType::CALL, 580);
    const std::string later = option("SPY", kNextFriday, OptionType::CALL, 580);
    single(holding(), friday, 1);
    single(holding(), later, 1);
    single(holding(), option("SPY", kFriday, OptionType::PUT, 570), 0); // closed: nothing to settle

    const auto close = model::option_expiry_time(kFriday);
    EXPECT_TRUE(engine_.expired_options("s", close - seconds(1)).empty());
    EXPECT_EQ(engine_.expired_options("s", close), std::vector<std::string>{friday});
    EXPECT_TRUE(engine_.expired_options("nobody", close).empty());

    const auto out = settle(engine_, kFriday, 575.0, portfolio("SPY", "SPY", true));
    ASSERT_EQ(out.size(), 1U);
    EXPECT_EQ(out[0].order.symbol, friday);
}

TEST_F(Settlement, AComboClosesWithItsLegs) {
    const std::string call = option("SPXW", kFriday, OptionType::CALL, 5800);
    const std::string put = option("SPXW", kFriday, OptionType::PUT, 5800);
    model::OptionPositionData combo("STRADDLE_SPXW_x");
    combo.combo_type = model::ComboType::STRADDLE;
    combo.quantity = -2; // two straddles sold
    combo.legs = {leg(call, -2), leg(put, -2)};
    holding().optionPositions[combo.symbol] = combo;

    const auto out = settle(engine_, kFriday, 5830.0, portfolio("SPXW", "SPX", false));
    ASSERT_EQ(out.size(), 1U);
    const Booked& s = out[0];
    EXPECT_TRUE(s.order.is_combo);
    EXPECT_EQ(s.order.symbol, combo.symbol);
    EXPECT_DOUBLE_EQ(s.order.volume, 2.0);
    EXPECT_DOUBLE_EQ(s.order.price, 30.0); // what a straddle was worth at the close
    ASSERT_EQ(s.fills.size(), 2U); // its legs: a fill is a contract's
    EXPECT_EQ(s.fills[0].symbol, call);
    EXPECT_EQ(s.fills[0].direction, Direction::LONG);
    EXPECT_DOUBLE_EQ(s.fills[0].price, 30.0);
    EXPECT_EQ(s.fills[1].symbol, put);
    EXPECT_DOUBLE_EQ(s.fills[1].price, 0.0);
    for (const auto& t : s.fills) {
        EXPECT_EQ(t.orderid, s.order.orderid);
    }
}

TEST_F(Settlement, ACalendarKeepsItsLaterLegOpen) {
    const std::string near = option("SPXW", kFriday, OptionType::CALL, 5800);
    const std::string far = option("SPXW", kNextFriday, OptionType::CALL, 5800);
    model::OptionPositionData combo("SPREAD_SPXW_x");
    combo.combo_type = model::ComboType::DIAGONAL_SPREAD;
    combo.quantity = 1;
    combo.legs = {leg(near, -1), leg(far, 1)};
    holding().optionPositions[combo.symbol] = combo;

    const auto out = settle(engine_, kFriday, 5790.0, portfolio("SPXW", "SPX", false));
    ASSERT_EQ(out.size(), 1U);
    ASSERT_EQ(out[0].fills.size(), 1U); // the near leg only; the calendar is still open
    EXPECT_EQ(out[0].fills[0].symbol, near);
}

TEST(PositionCheck, HoldingsSumByContractAndMeetTheBrokers) {
    const std::string call = option("SPY", kNextFriday, OptionType::CALL, 580);
    const std::string put = option("SPY", kNextFriday, OptionType::PUT, 580);
    core::PositionEngine engine;
    engine.get_create_strategy_holding("a");
    model::StrategyHolding& a = engine.get_holding("a");
    a.underlyingPosition.symbol = "SPY";
    a.underlyingPosition.quantity = -30;
    single(a, call, 1);
    engine.get_create_strategy_holding("b");
    model::OptionPositionData combo("STRADDLE_SPY_x");
    combo.quantity = 2;
    combo.legs = {leg(call, 2), leg(put, 2)};
    engine.get_holding("b").optionPositions[combo.symbol] = combo;
    EXPECT_EQ(engine.held_options(), (std::vector<std::string>{call, put}));
    EXPECT_FALSE(a.is_flat());

    model::PortfolioStructure portfolios; // SPY's among them: its shares and its options
    portfolios.ensure_portfolios_created();
    const std::string other = option("SPY", kNextFriday, OptionType::CALL, 590);
    const auto agree =
        engine.check_positions({{"SPY", -30}, {call, 3}, {put, 2}, {"AAPL", 10}}, portfolios);
    EXPECT_TRUE(std::ranges::none_of(agree, &core::PositionCheck::is_mismatch));

    const auto checks =
        engine.check_positions({{"SPY", -30}, {call, 2}, {"AAPL", 10}, {other, 1}}, portfolios);
    std::map<std::string, core::PositionCheck> by;
    for (const auto& c : checks) {
        by[c.symbol] = c;
    }
    ASSERT_EQ(by.size(), 5U);
    EXPECT_FALSE(by["SPY"].is_mismatch());
    EXPECT_TRUE(by[call].is_mismatch()); // short of one
    EXPECT_EQ(by[call].engine, 3);
    EXPECT_EQ(by[call].broker, 2);
    EXPECT_TRUE(by[put].is_mismatch());     // not at the broker
    EXPECT_TRUE(by[other].is_mismatch()); // one of ours, not held here
    EXPECT_FALSE(by["AAPL"].traded_here);      // traded by hand: listed, not compared
    EXPECT_FALSE(by["AAPL"].is_mismatch());
}

} // namespace
