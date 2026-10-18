/**
 * The pre-trade check (RiskEngine): each limit refuses what passes it and lets the rest through; a
 * strategy's working orders count toward what it would hold; halted, only an order that closes
 * goes; options settled in shares and expiring today are only closed in the last minutes before the
 * close, and the timer closes them there (of a combo, the contracts expiring); a limit set out of its
 * range is refused whole; the saved limits and halt come back as saved. A strategy past its
 * daily loss only closes until the next day, and all of them past theirs halt; Greek budgets, the
 * count of options held and the margin refuse what would take them further; stale quotes and a
 * broker not connected stop opening orders; without short options a sold leg must be covered.
 */
#include "core/bookkeeping/bookkeeping_engine.hpp"
#include "core/risk/risk_engine.hpp"
#include "model/portfolios.hpp"
#include "model/symbol.hpp"
#include "model/utility.hpp"
#include <chrono>
#include <cmath>
#include <cstddef>
#include <gtest/gtest.h>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono;
using model::Side;

const year_month_day kExpiry = 2099y / December / 18;

auto contract(const std::string& symbol, std::optional<model::OptionType> type = std::nullopt,
              double strike = 0.0) -> model::Contract {
    model::Contract c;
    c.symbol = symbol;
    c.trading_class = "SPY";
    if (!type) {
        c.product = model::Product::EQUITY;
        return c;
    }
    c.product = model::Product::OPTION;
    c.multiplier = 100;
    c.option_strike = strike;
    c.option_type = *type;
    c.option_expiry = model::option_expiry_time(kExpiry);
    c.option_underlying = "SPY";
    return c;
}

/** The portfolio's frame now, changed by `change` (its pricing kept as it is). */
template <typename F> void requote(model::Portfolio& p, F&& change) {
    model::PortfolioSnapshot s = p.snapshot();
    s.assign(p.frame());
    change(s);
    p.apply(s);
}

class RiskEngineTest : public ::testing::Test {
  protected:
    void SetUp() override {
        structure_.ensure_portfolio("SPY");
        structure_.load_contracts(
            std::vector{contract("SPY"), contract(call_, model::OptionType::CALL, 600)});
        model::Portfolio* spy = structure_.find_portfolio("SPY");
        spy->physical = true;
        requote(*spy, [](model::PortfolioSnapshot& q) {
            q.values(&model::Frame::bid)[0] = 1.9;
            q.values(&model::Frame::ask)[0] = 2.1;
            q.values(&model::Frame::delta)[0] = 0.5; // 50 shares a contract
            q.values(&model::Frame::gamma)[0] = 0.01;
            q.values(&model::Frame::vega)[0] = 0.3; // 30 USD a vol point a contract
            q.underlying_mark = 600.0;
            q.priced = true;
        });
        book_.ensure_holding("s");
    }

    /** An order for one contract, the call unless another is named: + buys, - sells; a limit
     * price is paid or received. */
    auto order(double signed_volume, model::OrderType type = model::OrderType::MARKET,
               double price = 0.0, const std::string& symbol = {}) const -> model::OrderRequest {
        model::OrderRequest req;
        req.symbol = symbol.empty() ? call_ : symbol;
        req.quantity = std::abs(signed_volume);
        req.type = type;
        req.price = signed_volume > 0 ? price : -price; // the net a unit
        req.legs = {{.ratio = 1,
                     .side = signed_volume > 0 ? Side::BUY : Side::SELL,
                     .symbol = req.symbol}};
        return req;
    }

    /** The strategy holds `quantity` of the call. */
    void hold(int quantity) {
        model::Fill t;
        t.fill_id = "T" + std::to_string(++fills_);
        t.order_id = "O" + std::to_string(fills_);
        t.symbol = call_;
        t.side = quantity > 0 ? Side::BUY : Side::SELL;
        t.price = 2.0;
        t.quantity = std::abs(quantity);
        book(t, nullptr);
    }

    /** The fill booked as "s"'s, its order (as given, or of its contract alone) done. */
    void book(const model::Fill& fill, const model::Order* order) {
        model::Order done;
        if (order != nullptr) {
            done = *order;
        } else {
            done.symbol = fill.symbol;
            done.legs = {{.ratio = 1, .side = fill.side, .symbol = fill.symbol}};
        }
        done.order_id = fill.order_id;
        done.status = model::Status::FILLED;
        if (book_.find_order(fill.order_id) == nullptr) {
            book_.add_order("s", done);
        }
        book_.book_fill(fill);
    }

    auto refusal(const model::OrderRequest& req, model::DateTime now = {})
        -> std::optional<std::string> {
        return risk_.refusal("s", "SPY", req, now);
    }

    const std::string call_ = model::option_symbol(
        {.root = "SPY", .expiry = kExpiry, .type = model::OptionType::CALL, .strike = 600});
    model::Portfolios structure_;
    core::BookkeepingEngine book_;
    core::RiskEngine risk_{nullptr, structure_, book_};
    int fills_ = 0;
};

TEST_F(RiskEngineTest, TheDefaultsLetAnOrdinaryOrderThrough) {
    EXPECT_EQ(refusal(order(10)), std::nullopt);
    EXPECT_EQ(refusal(order(-10)), std::nullopt); // short options allowed by default
}

TEST_F(RiskEngineTest, AnOrderOverItsSizeIsRefused) {
    risk_.set_limits({{"max_option_order", 5}});
    EXPECT_EQ(refusal(order(5)), std::nullopt);
    ASSERT_TRUE(refusal(order(6)));
    EXPECT_NE(refusal(order(6))->find("max_option_order"), std::string::npos);
}

TEST_F(RiskEngineTest, WorkingOrdersCountTowardWhatTheStrategyWouldHold) {
    risk_.set_limits({{"max_option_position", 6}});
    hold(3);
    model::Order working = order(2).create_order("W1");
    working.status = model::Status::WORKING;
    book_.add_order("s", working);
    EXPECT_TRUE(refusal(order(2)));          // 3 + 2 working + 2 = 7
    EXPECT_EQ(refusal(order(1)), std::nullopt); // 6
    EXPECT_EQ(refusal(order(-5)), std::nullopt); // toward zero: always
}

TEST_F(RiskEngineTest, NoShortOptionsMeansEverySoldLegIsCoveredByOneBought) {
    risk_.set_limits({{"short_options", 0}});
    hold(1);
    EXPECT_EQ(refusal(order(-1)), std::nullopt);
    ASSERT_TRUE(refusal(order(-2)));
    EXPECT_NE(refusal(order(-2))->find("uncovered"), std::string::npos);

    const auto contract = [](model::OptionType type, double strike) {
        return model::option_symbol(
            {.root = "SPY", .expiry = kExpiry, .type = type, .strike = strike});
    };
    // A call further out sold against the call held: a spread.
    model::OrderRequest sell =
        order(-1, model::OrderType::MARKET, 0.0, contract(model::OptionType::CALL, 605));
    EXPECT_EQ(refusal(sell), std::nullopt);
    sell = order(-1, model::OrderType::MARKET, 0.0,
                 contract(model::OptionType::PUT, 600)); // a call covers no put
    ASSERT_TRUE(refusal(sell));
    EXPECT_NE(refusal(sell)->find("puts"), std::string::npos);

    model::OrderRequest spread; // a put spread in one order: the put bought covers the put sold
    spread.symbol = "spread";
    spread.quantity = 2;
    spread.type = model::OrderType::MARKET;
    spread.legs = {{.ratio = 1,
                    .side = Side::BUY,
                    .symbol = contract(model::OptionType::PUT, 600)},
                   {.ratio = 1,
                    .side = Side::SELL,
                    .symbol = contract(model::OptionType::PUT, 590)}};
    EXPECT_EQ(refusal(spread), std::nullopt);
    spread.legs.erase(spread.legs.begin()); // its sold leg alone
    EXPECT_TRUE(refusal(spread));
}

TEST_F(RiskEngineTest, APremiumOverTheBudgetIsRefused) {
    risk_.set_limits({{"max_order_premium", 500}});
    EXPECT_EQ(refusal(order(2)), std::nullopt);  // 2 x 2.10 x 100 = 420
    EXPECT_TRUE(refusal(order(3)));              // 630
    EXPECT_EQ(refusal(order(-3)), std::nullopt); // selling pays nothing
    EXPECT_TRUE(refusal(order(3, model::OrderType::LIMIT, 2.0))); // 600 at its limit
}

TEST_F(RiskEngineTest, ALimitFarFromTheMidIsRefused) {
    risk_.set_limits({{"price_band", 0.2}, {"price_band_floor", 0.05}});
    EXPECT_EQ(refusal(order(1, model::OrderType::LIMIT, 2.3)), std::nullopt); // 0.3 <= 0.4
    EXPECT_TRUE(refusal(order(1, model::OrderType::LIMIT, 2.5)));             // 0.5 > 0.4
}

TEST_F(RiskEngineTest, AMarketOrderNeedsATightTwoSidedQuote) {
    risk_.set_limits({{"market_max_spread", 0.15}});
    EXPECT_EQ(refusal(order(1)), std::nullopt); // 0.20 wide on 2.00: 10%
    model::Portfolio& spy = *structure_.find_portfolio("SPY");
    requote(spy, [](model::PortfolioSnapshot& q) { q.values(&model::Frame::ask)[0] = 2.5; });
    EXPECT_TRUE(refusal(order(1)));
    requote(spy, [](model::PortfolioSnapshot& q) {
        q.values(&model::Frame::ask)[0] = 2.1;
        q.values(&model::Frame::bid)[0] = 0.0;
    });
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
    risk_.set_limits({{"flatten_minutes", 5}});
    hold(1);
    const model::DateTime close = model::option_expiry_time(kExpiry);
    EXPECT_EQ(refusal(order(1), close - minutes(10)), std::nullopt);
    EXPECT_TRUE(refusal(order(1), close - minutes(3)));
    EXPECT_EQ(refusal(order(-1), close - minutes(3)), std::nullopt);
    const model::Portfolio* spy = structure_.find_portfolio("SPY");
    EXPECT_TRUE(risk_.in_flatten_window(call_, spy, close - minutes(3)));
    EXPECT_FALSE(risk_.in_flatten_window(call_, spy, close - minutes(6)));
}

TEST_F(RiskEngineTest, ALimitOutOfItsRangeIsRefusedWhole) {
    EXPECT_THROW(risk_.set_limits({{"max_option_order", 0}}), std::invalid_argument);
    EXPECT_THROW(risk_.set_limits({{"max_option_order", 2.5}}),
                 std::invalid_argument); // whole numbers
    EXPECT_THROW(risk_.set_limits({{"no_such_limit", 1}}), std::invalid_argument);
    EXPECT_THROW(risk_.set_limits({{"max_option_order", 5}, {"short_options", 3}}),
                 std::invalid_argument);
    EXPECT_EQ(risk_.limit("max_option_order"), 100); // none set
}

TEST_F(RiskEngineTest, ARefusalIsCountedAndKept) {
    risk_.set_limits({{"max_option_order", 1}});
    EXPECT_FALSE(risk_.allow("s", "SPY", order(2), model::DateTime{}));
    EXPECT_TRUE(risk_.allow("s", "SPY", order(1), model::DateTime{}));
    EXPECT_EQ(risk_.refused(), 1U);
    ASSERT_EQ(risk_.recent_refusals().size(), 1U);
    EXPECT_NE(risk_.recent_refusals().front().find(call_), std::string::npos);
}

TEST_F(RiskEngineTest, TheFlattenWindowIsTheLastMinutesOfAShareSettledOptionsExpiryDay) {
    risk_.set_limits({{"flatten_minutes", 5}});
    const model::DateTime close = model::option_expiry_time(kExpiry);
    const model::Portfolio* spy = structure_.find_portfolio("SPY");
    EXPECT_TRUE(risk_.in_flatten_window(call_, spy, close - minutes(5))); // from its first minute
    EXPECT_FALSE(risk_.in_flatten_window(call_, spy, close - minutes(5) - seconds(1)));
    EXPECT_FALSE(risk_.in_flatten_window(call_, spy, close - hours(24))); // the day before
    EXPECT_FALSE(risk_.in_flatten_window("SPY", spy, close - minutes(1)));
    EXPECT_FALSE(risk_.in_flatten_window(call_, nullptr, close - minutes(1)));
    risk_.set_limits({{"flatten_minutes", 0}});
    EXPECT_FALSE(risk_.in_flatten_window(call_, spy, close - minutes(1))); // off
}

TEST_F(RiskEngineTest, AShareSettledOptionHeldIntoItsWindowIsToBeClosed) {
    risk_.set_limits({{"flatten_minutes", 5}});
    hold(2);
    const model::DateTime close = model::option_expiry_time(kExpiry);
    const model::Portfolio& spy = *structure_.find_portfolio("SPY");
    EXPECT_TRUE(risk_.positions_to_flatten("s", spy, close - minutes(10)).empty());
    const auto due = risk_.positions_to_flatten("s", spy, close - minutes(3));
    ASSERT_EQ(due.size(), 1U);
    ASSERT_EQ(due.at(call_).size(), 1U); // opened alone
    EXPECT_EQ(due.at(call_)[0].quantity, 2);
    EXPECT_TRUE(risk_.positions_to_flatten("nobody", spy, close - minutes(3)).empty());

    model::Portfolio cash("SPY"); // the same option settled in cash: held to the close
    cash.physical = false;
    EXPECT_TRUE(risk_.positions_to_flatten("s", cash, close - minutes(3)).empty());
}

TEST_F(RiskEngineTest, ACallSoldIsClosedBeforeTheCloseOfTheDayBeforeAnExDividendDay) {
    risk_.set_limits({{"flatten_minutes", 5}});
    const model::Portfolio& spy = *structure_.find_portfolio("SPY");
    const year_month_day today = 2025y / March / 20, ex = 2025y / March / 21; // Thursday, Friday
    const model::DateTime close = model::new_york_time(today, model::session_close(today));
    hold(-1); // a call sold, expiring long after
    EXPECT_TRUE(risk_.positions_to_flatten("s", spy, close - minutes(3)).empty()); // no dates given
    model::DailyTable dividends;
    dividends.set("SPY_ex_dividend", {{ex, 1.74}});
    risk_.use_daily(&dividends);
    EXPECT_TRUE(risk_.positions_to_flatten("s", spy, close - minutes(10)).empty()); // before the window
    EXPECT_EQ(risk_.positions_to_flatten("s", spy, close - minutes(3)).size(), 1U);
    EXPECT_TRUE(risk_.positions_to_flatten("s", spy, close - hours(24) - minutes(3)).empty()); // two days before
    model::Portfolio cash("SPY"); // settled in cash: never exercised early
    cash.physical = false;
    EXPECT_TRUE(risk_.positions_to_flatten("s", cash, close - minutes(3)).empty());
    hold(2); // now held long: nothing to fear from the dividend
    EXPECT_TRUE(risk_.positions_to_flatten("s", spy, close - minutes(3)).empty());
    risk_.use_daily(nullptr);
}

TEST_F(RiskEngineTest, NoCloseIsSentOverAWorkingOneNorForAFlatPosition) {
    risk_.set_limits({{"flatten_minutes", 5}});
    hold(1);
    const model::DateTime late = model::option_expiry_time(kExpiry) - minutes(3);
    const model::Portfolio& spy = *structure_.find_portfolio("SPY");
    model::Order closing = order(-1).create_order("C1");
    closing.status = model::Status::WORKING;
    book_.add_order("s", closing);
    EXPECT_TRUE(risk_.positions_to_flatten("s", spy, late).empty()); // the close sent a tick ago

    closing.status = model::Status::FILLED;
    closing.filled = 1;
    book_.update_order(closing, {});
    hold(-1);
    EXPECT_TRUE(risk_.positions_to_flatten("s", spy, late).empty()); // closed: flat
}

TEST_F(RiskEngineTest, OfAComboOnlyTheContractsExpiringInTheWindowAreClosed) {
    risk_.set_limits({{"flatten_minutes", 5}});
    const std::string later = model::option_symbol(
        {.root = "SPY", .expiry = 2099y / December / 31, .type = model::OptionType::PUT,
         .strike = 600});
    model::Order calendar;
    calendar.order_id = "K1";
    for (const std::string& leg : {call_, later}) {
        calendar.legs.push_back({.ratio = 1, .side = Side::BUY, .symbol = leg});
    }
    for (const std::string& leg : {call_, later}) {
        model::Fill t;
        t.fill_id = "K1" + leg;
        t.order_id = "K1";
        t.symbol = leg;
        t.side = Side::BUY;
        t.price = 2.0;
        t.quantity = 1;
        book(t, &calendar);
    }
    const auto due = risk_.positions_to_flatten(
        "s", *structure_.find_portfolio("SPY"), model::option_expiry_time(kExpiry) - minutes(3));
    ASSERT_EQ(due.size(), 1U);
    const auto& [combo, options] = *due.begin();
    EXPECT_EQ(combo, call_ + "|" + later); // what the call was opened in
    ASSERT_EQ(options.size(), 1U);
    EXPECT_EQ(options[0].symbol, call_);
    EXPECT_EQ(options[0].quantity, 1);
}

TEST_F(RiskEngineTest, SavedLimitsAndAHaltComeBackAsSaved) {
    risk_.set_limits({{"max_option_order", 7}, {"price_band", 0.25}});
    risk_.halt("a test", model::DateTime{} + seconds(1'800'000'000));
    std::unordered_map<std::string, std::string> saved;
    for (const auto& [name, value] : risk_.saved_limits()) {
        saved[name] = value;
    }
    saved["something.else"] = "x"; // another engine's setting

    core::RiskEngine restored{nullptr, structure_, book_};
    restored.restore_limits(saved);
    EXPECT_EQ(restored.limit("max_option_order"), 7);
    EXPECT_EQ(restored.limit("price_band"), 0.25);
    EXPECT_EQ(restored.limit("max_share_order"), 10'000);
    EXPECT_TRUE(restored.halted());
    EXPECT_EQ(restored.halt_reason(), "a test");
    EXPECT_EQ(restored.halted_at(), model::DateTime{} + seconds(1'800'000'000));
}

TEST_F(RiskEngineTest, ASavedLimitThatCannotBeUsedLeavesTheDefaults) {
    // Unreadable: that one keeps its default, the rest are used.
    risk_.restore_limits(
        {{"risk.limit.max_option_order", "many"}, {"risk.limit.short_options", "0"}});
    EXPECT_EQ(risk_.limit("max_option_order"), 100);
    EXPECT_EQ(risk_.limit("short_options"), 0);
    EXPECT_FALSE(risk_.halted());

    // Out of range or unknown (renamed since saved): that one keeps its default, the rest are used.
    core::RiskEngine out_of_range{nullptr, structure_, book_};
    out_of_range.restore_limits(
        {{"risk.limit.max_option_order", "7"}, {"risk.limit.short_options", "3"}});
    EXPECT_EQ(out_of_range.limit("max_option_order"), 7);
    EXPECT_EQ(out_of_range.limit("short_options"), 1);
    core::RiskEngine unknown{nullptr, structure_, book_};
    unknown.restore_limits(
        {{"risk.limit.max_option_order", "7"}, {"risk.limit.retired_limit", "1"}});
    EXPECT_EQ(unknown.limit("max_option_order"), 7);

    // A halt without a readable time is still a halt.
    core::RiskEngine halted{nullptr, structure_, book_};
    halted.restore_limits(
        {{"risk.halted", "1"}, {"risk.halt_reason", "r"}, {"risk.halted_at", ""}});
    EXPECT_TRUE(halted.halted());
    EXPECT_EQ(halted.halted_at(), model::DateTime{});
}

TEST_F(RiskEngineTest, AnOrderWithNothingToTradeIsRefused) {
    model::OrderRequest none = order(1);
    none.quantity = 0;
    EXPECT_EQ(refusal(none), "nothing to fill");
    model::OrderRequest no_legs = order(1);
    no_legs.legs = {{.ratio = 1, .side = Side::BUY, .symbol = ""}};
    EXPECT_EQ(refusal(no_legs), "nothing to fill");
}

TEST_F(RiskEngineTest, AnUnknownLimitReadsZeroAndANonFiniteOneIsRefused) {
    EXPECT_EQ(risk_.limit("no_such_limit"), 0.0);
    EXPECT_THROW(risk_.set_limits({{"price_band", std::nan("")}}), std::invalid_argument);
    EXPECT_EQ(risk_.limit("price_band"), 0.0);
}

TEST_F(RiskEngineTest, SharesHaveTheirOwnLimitsAndAreQuotedFromTheUnderlying) {
    model::Portfolio& spy = *structure_.find_portfolio("SPY");
    requote(spy, [](model::PortfolioSnapshot& q) {
        q.underlying_bid = 599.9;
        q.underlying_ask = 600.1;
        q.underlying_mark = 600.0;
    });
    risk_.set_limits(
        {{"max_share_order", 100}, {"max_share_position", 150}, {"market_max_spread", 0.01}});
    model::OrderRequest shares = order(101, model::OrderType::MARKET, 0.0, "SPY");
    ASSERT_TRUE(refusal(shares));
    EXPECT_NE(refusal(shares)->find("max_share_order"), std::string::npos);
    shares.quantity = 100;
    EXPECT_EQ(refusal(shares), std::nullopt);

    model::Fill bought;
    bought.fill_id = "S1";
    bought.order_id = "S1";
    bought.symbol = "SPY";
    bought.side = Side::BUY;
    bought.price = 600.0;
    bought.quantity = 100;
    book(bought, nullptr);
    shares.quantity = 60; // 160 held
    ASSERT_TRUE(refusal(shares));
    EXPECT_NE(refusal(shares)->find("max_share_position"), std::string::npos);
    shares = order(-60, model::OrderType::MARKET, 0.0, "SPY");
    EXPECT_EQ(refusal(shares), std::nullopt);

    // The underlying quoted one way only.
    requote(spy, [](model::PortfolioSnapshot& q) { q.underlying_bid = 0.0; });
    ASSERT_TRUE(refusal(shares));
    EXPECT_NE(refusal(shares)->find("no two-sided quote for SPY"), std::string::npos);
}

TEST_F(RiskEngineTest, WithoutAQuoteTheBandIsNotCheckedButAMarketOrderIsRefused) {
    risk_.set_limits({{"price_band", 0.1}});
    // A portfolio there is not: nothing quotes the call.
    EXPECT_EQ(risk_.refusal("s", "DIA", order(1, model::OrderType::LIMIT, 50.0), {}),
              std::nullopt);
    risk_.set_limits({{"market_max_spread", 0.15}});
    EXPECT_TRUE(risk_.refusal("s", "DIA", order(1), {}));
    EXPECT_EQ(refusal(order(1)), std::nullopt);
    requote(*structure_.find_portfolio("SPY"),
            [](model::PortfolioSnapshot& q) { q.values(&model::Frame::ask)[0] = 0.0; });
    EXPECT_TRUE(refusal(order(1))); // a bid without an ask
}

TEST_F(RiskEngineTest, WithoutAStrategyNothingIsHeldAndNothingCloses) {
    risk_.set_limits({{"short_options", 0}, {"max_option_position", 5}});
    EXPECT_EQ(risk_.refusal("", "SPY", order(-3), {}), std::nullopt); // no strategy to be short
    const auto over = risk_.refusal("", "SPY", order(6), {});
    ASSERT_TRUE(over);
    EXPECT_TRUE(over->starts_with("the order would hold 6")) << *over;
    risk_.halt("a test", {});
    EXPECT_TRUE(risk_.refusal("", "SPY", order(-1), {})); // holds nothing: nothing it closes
}

TEST_F(RiskEngineTest, AnotherClientsOrderCountsForNothing) {
    risk_.set_limits({{"max_option_position", 2}});
    book_.update_order(order(5).create_order("X1"), {});
    EXPECT_EQ(refusal(order(2)), std::nullopt);
}

TEST_F(RiskEngineTest, ASaleAtALimitPaysNoPremium) {
    risk_.set_limits({{"max_order_premium", 100}});
    EXPECT_EQ(refusal(order(-3, model::OrderType::LIMIT, 2.0)), std::nullopt);
    EXPECT_TRUE(refusal(order(1, model::OrderType::LIMIT, 2.0)));
}

TEST_F(RiskEngineTest, OnlyTheLatestRefusalsAreKept) {
    risk_.set_limits({{"max_option_order", 1}});
    for (std::size_t i = 0; i <= core::RiskEngine::kRecentRefusals; ++i) {
        EXPECT_FALSE(risk_.allow("s", "SPY", order(2), {}));
    }
    EXPECT_EQ(risk_.refused(), core::RiskEngine::kRecentRefusals + 1);
    EXPECT_EQ(risk_.recent_refusals().size(), core::RiskEngine::kRecentRefusals);
}

TEST_F(RiskEngineTest, AComboWhoseExpiringLegHasACloseWorkingIsNotClosedAgain) {
    risk_.set_limits({{"flatten_minutes", 5}});
    const std::string later = model::option_symbol(
        {.root = "SPY", .expiry = 2099y / December / 31, .type = model::OptionType::PUT,
         .strike = 600});
    model::Order calendar;
    calendar.order_id = "K1";
    for (const std::string& leg : {call_, later}) {
        calendar.legs.push_back({.ratio = 1, .side = Side::BUY, .symbol = leg});
    }
    for (const std::string& leg : {call_, later}) {
        model::Fill t;
        t.fill_id = "K1" + leg;
        t.order_id = "K1";
        t.symbol = leg;
        t.side = Side::BUY;
        t.price = 2.0;
        t.quantity = 1;
        book(t, &calendar);
    }
    const model::DateTime late = model::option_expiry_time(kExpiry) - minutes(3);
    const model::Portfolio& spy = *structure_.find_portfolio("SPY");
    ASSERT_EQ(risk_.positions_to_flatten("s", spy, late).size(), 1U);
    model::Order closing;
    closing.order_id = "K2";
    closing.symbol = "custom_close";
    closing.status = model::Status::WORKING;
    closing.legs = {{.ratio = 1, .side = Side::SELL, .symbol = call_}};
    book_.add_order("s", closing);
    EXPECT_TRUE(risk_.positions_to_flatten("s", spy, late).empty());
}

/** The call's mid moved to `mid`, the strategy's holding marked at it and checked. */
auto mark_at(core::BookkeepingEngine& positions, core::RiskEngine& risk, model::Portfolios& s,
             const std::string& call, double mid) -> void {
    model::Portfolio& spy = *s.find_portfolio("SPY");
    const std::uint32_t column = *spy.listing().column(call);
    requote(spy, [&](model::PortfolioSnapshot& q) {
        q.values(&model::Frame::bid)[column] = mid - 0.1;
        q.values(&model::Frame::ask)[column] = mid + 0.1;
    });
    positions.value_holding("s", spy);
    risk.measure_exposure("s", spy);
}

const model::DateTime kMonday = sys_days{2026y / October / 12} + 15h; // 11:00 New York
const model::DateTime kTuesday = kMonday + days(1);

TEST_F(RiskEngineTest, AStrategyPastItsDailyLossOnlyClosesUntilTheNextDay) {
    risk_.set_limits({{"max_strategy_daily_loss", 500}});
    hold(10);
    risk_.start_bar(kMonday);
    mark_at(book_, risk_, structure_, call_, 2.0);
    mark_at(book_, risk_, structure_, call_, 1.6); // 10 x 0.4 x 100 = 400 lost: under
    EXPECT_EQ(refusal(order(1), kMonday), std::nullopt);
    mark_at(book_, risk_, structure_, call_, 1.5); // 500
    ASSERT_TRUE(refusal(order(1), kMonday));
    EXPECT_NE(refusal(order(1), kMonday)->find("max_strategy_daily_loss"), std::string::npos);
    EXPECT_EQ(refusal(order(-4), kMonday), std::nullopt); // closing goes
    EXPECT_EQ(risk_.day_stops().size(), 1U);
    EXPECT_EQ(risk_.day_loss(), std::nullopt); // no global limit set

    risk_.start_bar(kTuesday); // the day starts from where the holding is
    mark_at(book_, risk_, structure_, call_, 1.5);
    EXPECT_TRUE(risk_.day_stops().empty());
    EXPECT_EQ(refusal(order(1), kTuesday), std::nullopt);
    EXPECT_TRUE(risk_.take_day_changed());
    EXPECT_FALSE(risk_.take_day_changed());
}

TEST_F(RiskEngineTest, OnADayTheMarketIsClosedTheMarksMovingStopNothing) {
    // A Saturday after a restart: the marks go from the last ones kept to the quotes now.
    risk_.set_limits({{"max_strategy_daily_loss", 500}, {"max_daily_loss", 300}});
    hold(10);
    risk_.start_bar(kMonday - days(2));
    mark_at(book_, risk_, structure_, call_, 2.0);
    mark_at(book_, risk_, structure_, call_, 1.0); // 1,000 "lost"
    EXPECT_TRUE(risk_.day_stops().empty());
    EXPECT_EQ(risk_.day_loss(), std::nullopt);
    risk_.start_bar(kMonday); // the trading day starts from where the holding is
    mark_at(book_, risk_, structure_, call_, 0.4); // 600 lost on Monday
    EXPECT_EQ(risk_.day_stops().size(), 1U);
}

TEST_F(RiskEngineTest, TheDaysLossOverEveryStrategyHalts) {
    risk_.set_limits({{"max_daily_loss", 300}});
    hold(10);
    risk_.start_bar(kMonday);
    mark_at(book_, risk_, structure_, call_, 2.0);
    mark_at(book_, risk_, structure_, call_, 1.8);
    EXPECT_EQ(risk_.day_loss(), std::nullopt);
    mark_at(book_, risk_, structure_, call_, 1.7);
    ASSERT_TRUE(risk_.day_loss());
    EXPECT_NE(risk_.day_loss()->find("max_daily_loss"), std::string::npos);
}

TEST_F(RiskEngineTest, TheDayStartsComeBackAsSaved) {
    hold(10);
    risk_.start_bar(kMonday);
    mark_at(book_, risk_, structure_, call_, 2.0);
    mark_at(book_, risk_, structure_, call_, 1.8); // 200 lost before the restart
    std::unordered_map<std::string, std::string> saved;
    for (const auto& [name, value] : risk_.saved_limits()) {
        saved[name] = value;
    }
    core::RiskEngine again{nullptr, structure_, book_};
    again.restore_limits(saved);
    again.set_limits({{"max_strategy_daily_loss", 500}});
    again.start_bar(kMonday + hours(2)); // the same day: the morning's start kept, not 1.8
    mark_at(book_, again, structure_, call_, 1.5);
    EXPECT_EQ(again.day_stops().size(), 1U);
    EXPECT_FALSE(again.take_day_changed());
}

TEST_F(RiskEngineTest, AGreekBudgetRefusesWhatTakesItFurtherFromZero) {
    // A contract: 50 shares x 600 = 30,000 USD.
    risk_.set_limits({{"max_strategy_delta_usd", 300'000}});
    hold(9);
    mark_at(book_, risk_, structure_, call_, 2.0);
    EXPECT_EQ(refusal(order(1)), std::nullopt);
    ASSERT_TRUE(refusal(order(2)));
    EXPECT_NE(refusal(order(2))->find("max_strategy_delta_usd"), std::string::npos);
    EXPECT_EQ(refusal(order(-2)), std::nullopt);
    risk_.set_limits({{"max_strategy_delta_usd", 0}, {"max_total_vega_usd", 300}}); // 30 a contract
    EXPECT_EQ(refusal(order(1)), std::nullopt);
    ASSERT_TRUE(refusal(order(2)));
    EXPECT_NE(refusal(order(2))->find("max_total_vega_usd"), std::string::npos);
}

TEST_F(RiskEngineTest, TheOptionsHeldAreCountedWithTheWorkingOrders) {
    risk_.set_limits({{"max_strategy_positions", 1}});
    EXPECT_EQ(refusal(order(1)), std::nullopt);
    hold(1);
    mark_at(book_, risk_, structure_, call_, 2.0);
    EXPECT_EQ(refusal(order(1)), std::nullopt); // the same contract
    const model::OrderRequest other = order(
        1, model::OrderType::MARKET, 0.0,
        model::option_symbol(
            {.root = "SPY", .expiry = kExpiry, .type = model::OptionType::PUT, .strike = 590}));
    ASSERT_TRUE(refusal(other));
    EXPECT_NE(refusal(other)->find("max_strategy_positions"), std::string::npos);
}

TEST_F(RiskEngineTest, MarginIsWhatTheOptionsSoldCanLose) {
    const model::Portfolio& spy = *structure_.find_portfolio("SPY");
    const std::string put600 = model::option_symbol(
        {.root = "SPY", .expiry = kExpiry, .type = model::OptionType::PUT, .strike = 600});
    const std::string put590 = model::option_symbol(
        {.root = "SPY", .expiry = kExpiry, .type = model::OptionType::PUT, .strike = 590});
    using Holding = std::vector<std::pair<std::string_view, double>>;
    EXPECT_DOUBLE_EQ(core::RiskEngine::margin(Holding{{call_, 3}}, spy), 0.0); // paid for
    EXPECT_DOUBLE_EQ(core::RiskEngine::margin(Holding{{put600, -1}}, spy), 60'000.0);
    EXPECT_DOUBLE_EQ(core::RiskEngine::margin(Holding{{put600, -1}, {put590, 1}}, spy),
                     1'000.0); // the spread's width
    EXPECT_DOUBLE_EQ(core::RiskEngine::margin(Holding{{call_, -1}}, spy), 12'000.0); // 20% of 600
    EXPECT_DOUBLE_EQ(core::RiskEngine::margin(Holding{{"SPY", 100}}, spy), 30'000.0);

    risk_.set_limits({{"max_margin", 50'000}});
    hold(-1); // a call sold: 12,000
    mark_at(book_, risk_, structure_, call_, 2.0);
    EXPECT_DOUBLE_EQ(risk_.margin("s"), 12'000.0);
    EXPECT_EQ(refusal(order(-3)), std::nullopt); // 48,000
    ASSERT_TRUE(refusal(order(-4)));
    EXPECT_NE(refusal(order(-4))->find("max_margin"), std::string::npos);
}

TEST_F(RiskEngineTest, StaleQuotesAndABrokerNotConnectedStopOpeningOrders) {
    hold(2);
    risk_.set_limits({{"max_quote_age_seconds", 60}});
    const model::DateTime quoted = structure_.find_portfolio("SPY")->datetime();
    EXPECT_EQ(refusal(order(1), quoted + seconds(30)), std::nullopt);
    ASSERT_TRUE(refusal(order(1), quoted + seconds(90)));
    EXPECT_NE(refusal(order(1), quoted + seconds(90))->find("max_quote_age_seconds"),
              std::string::npos);
    EXPECT_EQ(refusal(order(-1), quoted + seconds(90)), std::nullopt); // closing goes

    bool connected = false;
    risk_.watch_broker([&] { return connected; });
    ASSERT_TRUE(refusal(order(1), quoted));
    EXPECT_NE(refusal(order(1), quoted)->find("not connected"), std::string::npos);
    EXPECT_EQ(refusal(order(-1), quoted), std::nullopt);
    connected = true;
    EXPECT_EQ(refusal(order(1), quoted), std::nullopt);
}

} // namespace
