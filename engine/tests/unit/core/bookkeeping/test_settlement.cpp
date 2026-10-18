/**
 * Settlement and the positions check (BookkeepingEngine): expired options close out at their
 * intrinsic value in cash, or at nothing with the shares they deliver, each contract by itself; the
 * holdings summed by contract meet the broker's, each difference marked.
 */
#include "core/bookkeeping/bookkeeping_engine.hpp"
#include "model/portfolio.hpp"
#include "model/portfolios.hpp"
#include "model/symbol.hpp"
#include "model/utility.hpp"
#include <algorithm>
#include <chrono>
#include <gtest/gtest.h>
#include <map>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace {

using namespace std::chrono;
using model::Side;
using model::OptionType;

constexpr year_month_day kFriday{year{2025}, March, day{14}};
constexpr year_month_day kNextFriday{year{2025}, March, day{21}};

auto option(const std::string& root, year_month_day expiry, OptionType type, double strike)
    -> std::string {
    return model::option_symbol(
        {.root = root, .expiry = expiry, .type = type, .strike = strike});
}

auto single(model::StrategyHolding& h, const std::string& symbol, int quantity) {
    model::OptionPosition p(symbol);
    p.quantity = quantity;
    h.options.insert_or_assign(symbol, p);
}

/** A portfolio as settlement reads it: its underlying, and how its options settle. */
auto portfolio(const std::string& name, const std::string& underlying, bool physical)
    -> std::unique_ptr<model::Portfolio> {
    auto p = std::make_unique<model::Portfolio>(name);
    p->underlying_symbol = underlying;
    p->physical = physical;
    return p;
}

/** A portfolio whose underlying was last quoted at mid, at `at`. */
auto quoted(double mid, model::DateTime at) -> std::unique_ptr<model::Portfolio> {
    auto p = portfolio("SPY", "SPY", true);
    model::Contract spy;
    spy.symbol = "SPY";
    p->set_underlying(spy);
    model::PortfolioSnapshot s = p->snapshot();
    s.datetime = at;
    s.underlying_mark = mid;
    s.priced = true;
    p->apply(s);
    return p;
}

using Booked = core::BookkeepingEngine::Settlement;
using Unsettled = std::unordered_map<std::string, std::vector<std::string>>;

/** Settle strategy "s" of the engine. */
auto settle(const core::BookkeepingEngine& engine, year_month_day expiry, double price,
            const model::Portfolio& p) -> std::vector<Booked> {
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
    void SetUp() override { engine_.ensure_holding("s"); }
    model::StrategyHolding& holding() { return engine_.ensure_holding("s"); }

    core::BookkeepingEngine engine_;
};

TEST_F(Settlement, CashClosesAtIntrinsicValue) {
    const std::string call = option("SPXW", kFriday, OptionType::CALL, 5800);
    const std::string put = option("SPXW", kFriday, OptionType::PUT, 5900);
    const std::string otm = option("SPXW", kFriday, OptionType::CALL, 6000);
    single(holding(), call, 2);
    single(holding(), put, -1);
    single(holding(), otm, 3);

    const auto out = settle(engine_, kFriday, 5850.0, *portfolio("SPXW", "SPX", false));
    ASSERT_EQ(out.size(), 3U); // no shares change hands
    const auto& c = for_symbol(out, call);
    EXPECT_EQ(c.order.status, model::Status::FILLED);
    EXPECT_EQ(c.order.reference, "Settlement");
    ASSERT_EQ(c.fills.size(), 1U);
    EXPECT_EQ(c.fills[0].side, Side::SELL);
    EXPECT_EQ(c.fills[0].quantity, 2.0);
    EXPECT_DOUBLE_EQ(c.fills[0].price, 50.0);
    EXPECT_EQ(c.fills[0].datetime, model::option_expiry_time(kFriday));
    const auto& p = for_symbol(out, put);
    EXPECT_EQ(p.fills[0].side, Side::BUY); // a short put bought back
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

    const auto out = settle(engine_, kFriday, 585.0, *portfolio("SPY", "SPY", true));
    for (const std::string& o : {long_call, short_put, otm_call, by_a_hair}) {
        EXPECT_DOUBLE_EQ(for_symbol(out, o).fills.at(0).price, 0.0) << o; // the option is gone
    }
    std::vector<model::Fill> shares;
    for (const auto& b : out) {
        if (b.order.symbol == "SPY") {
            shares.push_back(b.fills.at(0));
        }
    }
    ASSERT_EQ(shares.size(), 2U);
    std::ranges::sort(shares, {}, &model::Fill::price);
    EXPECT_EQ(shares[0].side, Side::BUY); // the long call: 100 bought at 580
    EXPECT_EQ(shares[0].quantity, 100.0);
    EXPECT_DOUBLE_EQ(shares[0].price, 580.0);
    EXPECT_EQ(shares[1].side, Side::BUY); // the short put, assigned: 200 bought at 590
    EXPECT_EQ(shares[1].quantity, 200.0);
    EXPECT_DOUBLE_EQ(shares[1].price, 590.0);
}

TEST_F(Settlement, OnlyTheExpiryAskedForAndOnlyOncePastTheClose) {
    const std::string friday = option("SPY", kFriday, OptionType::CALL, 580);
    const std::string later = option("SPY", kNextFriday, OptionType::CALL, 580);
    single(holding(), friday, 1);
    single(holding(), later, 1);
    single(holding(), option("SPY", kFriday, OptionType::PUT, 570), 0); // closed: nothing to settle

    const auto close = model::option_expiry_time(kFriday);
    // No portfolio, no closing price: the expired wait, unsettled.
    EXPECT_TRUE(engine_.expiries_to_settle("s", nullptr, close - seconds(1)).empty());
    EXPECT_TRUE(engine_.unsettled_options().empty());
    EXPECT_TRUE(engine_.expiries_to_settle("s", nullptr, close).empty());
    EXPECT_EQ(engine_.unsettled_options().at("s"), std::vector<std::string>{friday});
    EXPECT_TRUE(engine_.expiries_to_settle("nobody", nullptr, close).empty());

    const auto out = settle(engine_, kFriday, 575.0, *portfolio("SPY", "SPY", true));
    ASSERT_EQ(out.size(), 1U);
    EXPECT_EQ(out[0].order.symbol, friday);
}

TEST_F(Settlement, EachContractOfAComboSettlesByItself) {
    const std::string call = option("SPXW", kFriday, OptionType::CALL, 5800);
    const std::string put = option("SPXW", kFriday, OptionType::PUT, 5800);
    single(holding(), call, -2); // two straddles sold
    single(holding(), put, -2);

    const auto out = settle(engine_, kFriday, 5830.0, *portfolio("SPXW", "SPX", false));
    ASSERT_EQ(out.size(), 2U);
    for (const auto& [symbol, price] : {std::pair{call, 30.0}, std::pair{put, 0.0}}) {
        const Booked& s = for_symbol(out, symbol);
        EXPECT_FALSE(s.order.is_combo());
        ASSERT_EQ(s.fills.size(), 1U);
        EXPECT_EQ(s.fills[0].side, Side::BUY); // bought back
        EXPECT_DOUBLE_EQ(s.fills[0].quantity, 2.0);
        EXPECT_DOUBLE_EQ(s.fills[0].price, price);
    }
}

TEST_F(Settlement, OnlyTheContractsOpenAndExpiringNowSettle) {
    const std::string call = option("SPXW", kFriday, OptionType::CALL, 5800);
    const std::string put = option("SPXW", kFriday, OptionType::PUT, 5800);
    const std::string far = option("SPXW", kNextFriday, OptionType::CALL, 5800);
    single(holding(), call, 1);
    single(holding(), put, 0); // sold back already
    single(holding(), far, -1); // a calendar's later leg: still open

    const auto out = settle(engine_, kFriday, 5850.0, *portfolio("SPXW", "SPX", false));
    ASSERT_EQ(out.size(), 1U);
    EXPECT_EQ(out[0].order.symbol, call);
    EXPECT_DOUBLE_EQ(out[0].fills[0].price, 50.0);
}

TEST_F(Settlement, AnExpiryQuotedAtItsCloseSettlesAndOneWithoutAClosingPriceWaits) {
    const std::string friday = option("SPY", kFriday, OptionType::CALL, 580);
    const std::string later = option("SPY", kNextFriday, OptionType::PUT, 570);
    single(holding(), friday, 1);
    single(holding(), later, -1);
    const auto close = model::option_expiry_time(kNextFriday);
    const auto spy = quoted(585.0, close + minutes(5)); // a week after Friday's

    const auto due = engine_.expiries_to_settle("s", spy.get(), close + minutes(10));
    ASSERT_EQ(due.size(), 1U);
    EXPECT_EQ(due[0].first, kNextFriday);
    EXPECT_DOUBLE_EQ(due[0].second, 585.0);
    EXPECT_EQ(engine_.unsettled_options(), (Unsettled{{"s", {friday}}}));
}

TEST_F(Settlement, AQuoteOutsideTheWindowIsNoClosingPrice) {
    const std::string friday = option("SPY", kFriday, OptionType::CALL, 580);
    single(holding(), friday, 1);
    const auto close = model::option_expiry_time(kFriday);
    const auto window = minutes(15); // the engine's settlement window

    const auto before = quoted(585.0, close - window);
    EXPECT_EQ(engine_.expiries_to_settle("s", before.get(), close).size(), 1U);
    EXPECT_TRUE(engine_.unsettled_options().empty());

    const auto morning = quoted(585.0, close + window + minutes(1));
    EXPECT_TRUE(engine_.expiries_to_settle("s", morning.get(), close + hours(16)).empty());
    EXPECT_EQ(engine_.unsettled_options(), (Unsettled{{"s", {friday}}}));

    const auto unquoted = quoted(0.0, close);
    EXPECT_TRUE(engine_.expiries_to_settle("s", unquoted.get(), close).empty());
}

TEST_F(Settlement, AnUnsettledOptionGoesOnceTheHoldingIsFlat) {
    const std::string friday = option("SPY", kFriday, OptionType::CALL, 580);
    single(holding(), friday, 2);
    const auto after = model::option_expiry_time(kFriday) + hours(1);
    EXPECT_TRUE(engine_.expiries_to_settle("s", nullptr, after).empty());
    EXPECT_EQ(engine_.unsettled_options(), (Unsettled{{"s", {friday}}}));

    holding().options.at(friday).quantity = 0; // settled by hand
    EXPECT_TRUE(engine_.expiries_to_settle("s", nullptr, after).empty());
    EXPECT_TRUE(engine_.unsettled_options().empty());
}

TEST_F(Settlement, WithoutAPortfolioEveryExpiredOptionWaitsUntilTheHoldingIsRemoved) {
    const std::string friday = option("SPY", kFriday, OptionType::CALL, 580);
    const std::string later = option("SPY", kNextFriday, OptionType::PUT, 570);
    const std::string open = option("SPY", sys_days{kNextFriday} + days{7}, OptionType::CALL, 580);
    single(holding(), friday, 1);
    single(holding(), later, 1);
    single(holding(), open, 1); // not expired: never waits

    const auto after = model::option_expiry_time(kNextFriday) + minutes(1);
    EXPECT_TRUE(engine_.expiries_to_settle("s", nullptr, after).empty());
    EXPECT_EQ(engine_.unsettled_options(), (Unsettled{{"s", {friday, later}}}));

    engine_.remove_strategy("s");
    EXPECT_TRUE(engine_.unsettled_options().empty());
    EXPECT_TRUE(engine_.expiries_to_settle("s", nullptr, after).empty());
}

TEST_F(Settlement, NothingSettlesForAStrategyThatHoldsNothing) {
    EXPECT_TRUE(engine_.settle_expiry("nobody", kFriday, 5850.0,
                                      *portfolio("SPXW", "SPX", false))
                    .empty());
}

TEST(BrokerCheckRow, HoldingsSumByContractAndMeetTheBrokers) {
    const std::string call = option("SPY", kNextFriday, OptionType::CALL, 580);
    const std::string put = option("SPY", kNextFriday, OptionType::PUT, 580);
    core::BookkeepingEngine engine;
    model::StrategyHolding& a = engine.ensure_holding("a");
    a.shares.symbol = "SPY";
    a.shares.quantity = -30;
    single(a, call, 1);
    single(engine.ensure_holding("b"), call, 2); // a straddle
    single(engine.ensure_holding("b"), put, 2);
    EXPECT_EQ(engine.held_options(), (std::vector<std::string>{call, put}));
    EXPECT_FALSE(a.is_flat());

    model::Portfolios portfolios; // SPY's among them: its shares and its options
    portfolios.ensure_portfolios_created();
    const std::string other = option("SPY", kNextFriday, OptionType::CALL, 590);
    const auto agree =
        engine.compare_with_broker({{"SPY", -30}, {call, 3}, {put, 2}, {"AAPL", 10}}, portfolios);
    EXPECT_TRUE(std::ranges::none_of(agree, &core::BrokerCheckRow::is_mismatch));

    const auto checks =
        engine.compare_with_broker({{"SPY", -30}, {call, 2}, {"AAPL", 10}, {other, 1}}, portfolios);
    std::map<std::string, core::BrokerCheckRow> by;
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
