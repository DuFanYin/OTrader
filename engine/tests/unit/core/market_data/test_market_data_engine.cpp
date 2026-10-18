/**
 * Market data (MarketDataEngine): the chains strategies subscribe become the instruments a source
 * streams (all of them, or a window around the money that fits the source's lines), and quotes
 * become snapshots in the portfolio's column order after each batch, priced (only what moved priced
 * again, to the same values as pricing all). The engine starts and stops its source. In the live
 * engine a strategy finds its 0DTE chain at init, before any quote has arrived, and what it
 * subscribes is what gets streamed.
 */
#include "core/strategy/strategy_engine.hpp"
#include "model/portfolios.hpp"
#include "adapters/database/sqlite_database.hpp"
#include "core/market_data/market_data_engine.hpp"
#include "assembly/live.hpp"
#include "strategy/strategy_registry.hpp"
#include "strategy/strategy.hpp"
#include "model/symbol.hpp"
#include "model/utility.hpp"
#include "utilities/logger.hpp"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <gtest/gtest.h>
#include <initializer_list>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

using namespace std::chrono;
using core::Instrument;
using core::MarketDataEngine;
using core::QuoteField;

const std::string kChain = "SPY_20991218";
// When the test's quotes were taken: a snapshot is as of its latest one.
const model::DateTime kQuoted = sys_days{2099y / December / 18} + 15h;
const std::string kOtherChain = "SPY_20991219";
constexpr int kLowStrike = 490;
constexpr int kHighStrike = 520;
constexpr std::size_t kStrikes = kHighStrike - kLowStrike + 1;

auto underlying() -> model::Contract {
    model::Contract c;
    c.symbol = "SPY";
    c.product = model::Product::EQUITY;
    c.trading_class = "SPY";
    c.con_id = 756733;
    return c;
}

auto option(year_month_day expiry, model::OptionType type, double strike)
    -> model::Contract {
    const bool call = type == model::OptionType::CALL;
    model::Contract c;
    c.symbol =
        model::option_symbol({.root = "SPY", .expiry = expiry, .type = type, .strike = strike});
    c.product = model::Product::OPTION;
    c.multiplier = 100;
    c.trading_class = "SPY";
    c.con_id = static_cast<int>(strike * 10) + (call ? 0 : 1);
    c.option_strike = strike;
    c.option_type = type;
    c.option_expiry = model::option_expiry_time(expiry);
    c.option_underlying = "SPY";
    return c;
}

/** The call strikes among the instruments, in order. */
auto call_strikes(const std::vector<Instrument>& instruments) -> std::vector<double> {
    std::vector<double> out;
    for (const Instrument& i : instruments) {
        if (i.contract->option_type == model::OptionType::CALL) {
            out.push_back(*i.contract->option_strike);
        }
    }
    std::ranges::sort(out);
    return out;
}

auto strike_range(int first, int last) -> std::vector<double> {
    std::vector<double> out;
    for (int k = first; k <= last; ++k) {
        out.push_back(k);
    }
    return out;
}

/** SPY's underlying, and a call and a put at every strike from low to high of each expiry. */
auto spy_contracts(std::initializer_list<year_month_day> expiries, int low, int high)
    -> std::vector<model::Contract> {
    std::vector<model::Contract> out{underlying()};
    for (const year_month_day expiry : expiries) {
        for (int k = low; k <= high; ++k) {
            for (const auto type : {model::OptionType::CALL, model::OptionType::PUT}) {
                out.push_back(option(expiry, type, k));
            }
        }
    }
    return out;
}

/** A snapshot of SPY at kQuoted, spot 500, each option a little over its intrinsic value. */
auto quoted(const model::Portfolio& spy) -> model::PortfolioSnapshot {
    model::PortfolioSnapshot s = spy.snapshot();
    s.datetime = kQuoted;
    s.underlying_bid = 499.9;
    s.underlying_ask = 500.1;
    const model::Listing& listing = spy.listing();
    for (std::size_t i = 0; i < listing.size(); ++i) {
        const double k = listing.strike(i);
        const bool call = listing.type(i) == model::OptionType::CALL;
        const double mid = std::max(call ? 500.0 - k : k - 500.0, 0.0) + 1.5;
        s.values(&model::Frame::bid)[i] = mid - 0.05;
        s.values(&model::Frame::ask)[i] = mid + 0.05;
    }
    return s;
}

auto copy(std::span<const double> values) -> std::vector<double> {
    return {values.begin(), values.end()};
}

/** SPY with two chains of calls and puts at every strike from 490 to 520. */
class MarketDataEngineTest : public ::testing::Test {
  protected:
    void SetUp() override {
        structure_.ensure_portfolio("SPY");
        structure_.load_contracts(spy_contracts({2099y / December / 18, 2099y / December / 19},
                                                kLowStrike, kHighStrike));
    }

    auto book(milliseconds interval = 1s) -> std::unique_ptr<MarketDataEngine> {
        return std::make_unique<MarketDataEngine>(
            nullptr, structure_, nullptr,
            [this](const model::PortfolioSnapshot& s) { snapshots_.push_back(s); }, interval);
    }

    /** Quote the underlying at bid / ask. */
    static void price(MarketDataEngine& book, const Instrument& under, double bid, double ask) {
        book.quote(under.slot, QuoteField::Bid, bid, kQuoted);
        book.quote(under.slot, QuoteField::Ask, ask, kQuoted);
    }

    model::Portfolios structure_;
    std::vector<model::PortfolioSnapshot> snapshots_;
};

/** Values the same to rounding: what rescaling gives against pricing again. */
void expect_close(std::span<const double> got, std::span<const double> want, const char* what) {
    ASSERT_EQ(got.size(), want.size()) << what;
    for (std::size_t i = 0; i < got.size(); ++i) {
        EXPECT_NEAR(got[i], want[i], 1e-12 * std::max(1.0, std::abs(want[i]))) << what << " at " << i;
    }
}

/** A chain big enough that pricing runs on the shared pool: priced again after the underlying
 * alone moved (each option's own flags read on the pool's threads), it is what pricing it all
 * gives (delta and gamma rescaled: to rounding). */
TEST(PortfolioPricing, BigChainRepricedOnPoolAfterUnderlyingMove) {
    model::Portfolios structure;
    structure.ensure_portfolio("SPY");
    structure.load_contracts(spy_contracts({2099y / December / 18, 2099y / December / 19}, 400, 599));
    const model::Portfolio* spy = structure.find_portfolio("SPY");
    const std::size_t n = spy->listing().size();
    ASSERT_GT(n, 256U); // above the pool's inline threshold

    model::PortfolioSnapshot s = quoted(*spy);
    spy->price_snapshot(s);
    ASSERT_TRUE(s.priced);

    s.underlying_bid = 501.9; // the underlying alone moves
    s.underlying_ask = 502.1;
    const std::vector<std::uint8_t> unchanged(n, 0);
    spy->price_snapshot(s, unchanged, true);
    model::PortfolioSnapshot whole = s;
    whole.priced = false;
    spy->price_snapshot(whole);
    EXPECT_EQ(copy(s.iv), copy(whole.iv));
    expect_close(s.delta, whole.delta, "delta");
    EXPECT_EQ(copy(s.forward), copy(whole.forward));
}

/** The underlying alone moving rescales delta and gamma (the forward comes from the options' own
 * quotes): what pricing it all gives, to rounding. Live, an expiry's time moves in steps of
 * kLiveTimeStep while more than an hour is left, and every second under it. A same-day chain and a
 * three-day one. */
TEST(PortfolioPricing, AnUnderlyingTickRescalesAndTimeMovesInSteps) {
    model::Portfolios structure;
    structure.ensure_portfolio("SPY");
    structure.load_contracts(spy_contracts({2099y / December / 18, 2099y / December / 21}, 480, 520));
    const model::Portfolio* spy = structure.find_portfolio("SPY");

    model::PortfolioSnapshot s = quoted(*spy); // six hours before the same-day chain's close
    spy->price_snapshot(s);
    const std::vector<std::uint8_t> unchanged(spy->listing().size(), 0);
    const auto step = model::Portfolio::kLiveTimeStep;
    auto whole = [&] {
        model::PortfolioSnapshot w = s;
        w.priced = false;
        spy->price_snapshot(w);
        return w;
    };

    for (const double move : {0.25, 3.0, -7.5}) { // small and large moves alike: exact
        s.underlying_bid += move;
        s.underlying_ask += move;
        spy->price_snapshot(s, unchanged, true, step);
        const model::PortfolioSnapshot w = whole();
        EXPECT_EQ(copy(s.iv), copy(w.iv));
        expect_close(s.delta, w.delta, "delta");
        expect_close(s.gamma, w.gamma, "gamma");
        EXPECT_EQ(copy(s.theta), copy(w.theta));
        EXPECT_EQ(copy(s.vega), copy(w.vega));
    }

    // Five seconds: the time stays as priced; eleven: priced again.
    const std::vector<double> years = copy(s.expiry_years);
    s.datetime += 5s;
    spy->price_snapshot(s, unchanged, false, step);
    EXPECT_EQ(copy(s.expiry_years), years);
    s.datetime += 6s;
    spy->price_snapshot(s, unchanged, false, step);
    EXPECT_NE(copy(s.expiry_years), years);
    EXPECT_EQ(copy(s.expiry_years), copy(whole().expiry_years));
    EXPECT_EQ(copy(s.iv), copy(whole().iv));

    // Under an hour before the same-day close, every second counts there.
    s.datetime = sys_days{2099y / December / 18} + 20h + 30min; // 15:30 New York
    spy->price_snapshot(s, unchanged, false, step);
    const std::vector<double> late = copy(s.expiry_years);
    s.datetime += 1s;
    spy->price_snapshot(s, unchanged, false, step);
    EXPECT_NE(s.expiry_years[0], late[0]);
    EXPECT_EQ(s.expiry_years[1], late[1]); // the three-day chain: within its step
}

/** SPXW: options on SPX, an index that does not trade, hedged with SPY. */
TEST(MarketDataEngineHedge, AnIndexPortfolioAlsoStreamsWhatItHedgesWith) {
    model::Portfolios structure;
    structure.ensure_portfolios_created(); // SPY and SPXW (core/portfolios.hpp)
    model::Contract spx;
    spx.symbol = "SPX";
    spx.product = model::Product::INDEX;
    spx.con_id = 416904;
    // SPY: the SPY portfolio's, and SPXW's hedge.
    model::Contract call = option(2099y / December / 18, model::OptionType::CALL, 6000);
    call.symbol = model::option_symbol({.root = "SPXW",
                                            .expiry = 2099y / December / 18,
                                            .type = model::OptionType::CALL,
                                            .strike = 6000});
    call.trading_class = "SPXW";
    call.option_underlying = "SPX";
    structure.load_contracts(std::vector{spx, underlying(), call});

    std::vector<model::PortfolioSnapshot> snapshots;
    MarketDataEngine book(
        nullptr, structure, nullptr,
        [&](const model::PortfolioSnapshot& s) { snapshots.push_back(s); }, 1s);
    book.subscribe_chains("s", std::vector<std::string>{"SPXW_20991218"});
    const std::vector<Instrument> streamed = book.instruments(0);
    const auto slot_of = [&](const std::string& symbol) {
        const auto it = std::ranges::find(streamed, symbol,
                                          [](const Instrument& i) { return i.contract->symbol; });
        EXPECT_NE(it, streamed.end()) << symbol;
        return it != streamed.end() ? it->slot : core::QuoteSlot{};
    };
    EXPECT_EQ(slot_of("SPX").option, core::QuoteSlot::kUnderlying);
    const core::QuoteSlot spy = slot_of("SPY");
    EXPECT_EQ(spy.option, core::QuoteSlot::kHedge);
    EXPECT_EQ(streamed.size(), 3U); // SPX, SPY, the call

    book.quote(slot_of("SPX"), QuoteField::Last, 6000.0, kQuoted);
    book.quote(spy, QuoteField::Bid, 599.99, kQuoted);
    book.quote(spy, QuoteField::Ask, 600.01, kQuoted);
    book.publish();
    ASSERT_EQ(snapshots.size(), 1U);
    EXPECT_EQ(snapshots[0].portfolio_name, "SPXW");
    EXPECT_DOUBLE_EQ(snapshots[0].hedge_bid, 599.99);
    EXPECT_DOUBLE_EQ(snapshots[0].hedge_mark, 600.0);

    // Applied: a share of SPY counts as a tenth of a unit of SPX.
    model::Portfolio* spxw = structure.find_portfolio("SPXW");
    spxw->apply(snapshots[0]);
    ASSERT_TRUE(spxw->hedge());
    EXPECT_EQ(spxw->hedge()->symbol(), "SPY");
    EXPECT_DOUBLE_EQ(spxw->hedge()->theo_delta(), 0.1);
}

TEST_F(MarketDataEngineTest, StreamsTheSubscribedChainsAndTheirUnderlying) {
    auto md = book();
    EXPECT_TRUE(md->instruments(0).empty());

    md->subscribe_chains("s1", std::vector{kChain});
    const std::vector<Instrument> one = md->instruments(0);
    ASSERT_EQ(one.size(), 1 + 2 * kStrikes);
    EXPECT_EQ(one[0].contract->symbol, "SPY");
    EXPECT_LT(one[0].slot.option, 0);
    EXPECT_TRUE(std::ranges::all_of(one.begin() + 1, one.end(), [](const Instrument& i) {
        return i.contract->symbol.starts_with("SPY-20991218-") && i.slot.option >= 0;
    }));

    md->subscribe_chains("s2", std::vector{kChain, kOtherChain}); // kChain once, not twice
    EXPECT_EQ(md->instruments(0).size(), 1 + 4 * kStrikes);
    md->unsubscribe_chains("s2");
    EXPECT_EQ(md->instruments(0).size(), 1 + 2 * kStrikes);
    md->unsubscribe_chains("s1");
    EXPECT_TRUE(md->instruments(0).empty());
}

TEST_F(MarketDataEngineTest, AWindowAroundTheMoneyFitsTheLines) {
    auto md = book();
    md->subscribe_chains("s1", std::vector{kChain});
    // 21 lines: the underlying, and 10 strikes of a call and a put each.
    const std::vector<Instrument> before = md->instruments(21);
    ASSERT_EQ(before.size(), 1U); // no price to center on yet
    const Instrument under = before[0];

    price(*md, under, 504.9, 505.1);
    const std::vector<Instrument> window = md->instruments(21);
    EXPECT_EQ(window.size(), 21U);
    EXPECT_EQ(call_strikes(window), strike_range(500, 509));

    // Within a quarter of the window from its center the window stays; further, it follows.
    price(*md, under, 506.9, 507.1);
    EXPECT_EQ(call_strikes(md->instruments(21)), strike_range(500, 509));
    price(*md, under, 507.9, 508.1);
    EXPECT_EQ(call_strikes(md->instruments(21)), strike_range(503, 512));
    // At the edge of the chain it stays whole.
    price(*md, under, 489.0, 489.2);
    EXPECT_EQ(call_strikes(md->instruments(21)), strike_range(490, 499));

    // Two chains share the lines.
    md->subscribe_chains("s2", std::vector{kOtherChain});
    EXPECT_EQ(md->instruments(21).size(), 1U + 2 * 2 * 5);
}

TEST_F(MarketDataEngineTest, QuotesBecomeSnapshotsInColumnOrderAsTheyChange) {
    auto md = book(1s);
    md->subscribe_chains("s1", std::vector{kChain});
    const std::vector<Instrument> all = md->instruments(0);
    const Instrument under = all[0];
    const auto call = std::ranges::find_if(
        all, [](const Instrument& i) { return i.contract->symbol == "SPY-20991218-C-505"; });
    ASSERT_NE(call, all.end());
    md->quote(call->slot, QuoteField::Bid, 1.0, kQuoted);
    md->quote(call->slot, QuoteField::Ask, 1.2, kQuoted + 2s);
    md->quote(under.slot, QuoteField::Last, 505.0, kQuoted); // a last value only, as an index has

    md->publish();
    ASSERT_EQ(snapshots_.size(), 1U);
    const model::PortfolioSnapshot& s = snapshots_[0];
    EXPECT_EQ(s.portfolio_name, "SPY");
    const model::Listing& listing = structure_.find_portfolio("SPY")->listing();
    ASSERT_EQ(s.bid.size(), listing.size());
    const auto column = static_cast<std::size_t>(call->slot.option);
    EXPECT_EQ(listing.contract(column).symbol, call->contract->symbol);
    EXPECT_DOUBLE_EQ(s.bid[column], 1.0);
    EXPECT_DOUBLE_EQ(s.ask[column], 1.2);
    EXPECT_DOUBLE_EQ(s.underlying_mark, 505.0);
    EXPECT_EQ(s.datetime, kQuoted + 2s); // as of the latest quote, not a clock

    EXPECT_TRUE(s.priced);

    md->publish();
    EXPECT_EQ(snapshots_.size(), 1U); // nothing changed, nothing sent

    md->clear(call->slot);
    md->publish();
    ASSERT_EQ(snapshots_.size(), 2U);
    EXPECT_DOUBLE_EQ(snapshots_[1].bid[column], 0.0);

    // What to stream is planned once an interval.
    const auto t0 = steady_clock::time_point{} + 1h;
    EXPECT_TRUE(md->replan_due(t0));
    EXPECT_FALSE(md->replan_due(t0 + 500ms)); // not yet
    EXPECT_TRUE(md->replan_due(t0 + 1s));
}

TEST_F(MarketDataEngineTest, OnlyWhatMovedIsPricedAgainToTheSameValues) {
    auto md = book();
    md->subscribe_chains("s1", std::vector{kChain, kOtherChain});
    const std::vector<Instrument> all = md->instruments(0);
    const Instrument under = all[0];
    const auto quote_option = [&](const Instrument& i, double spot, double extra,
                                  model::DateTime at) {
        const double k = *i.contract->option_strike;
        const bool call = i.contract->option_type == model::OptionType::CALL;
        const double mid = std::max(call ? spot - k : k - spot, 0.0) + 1.5 + extra;
        md->quote(i.slot, QuoteField::Bid, mid - 0.05, at);
        md->quote(i.slot, QuoteField::Ask, mid + 0.05, at);
    };
    // Each snapshot as priced is what pricing it all from its quotes gives (to rounding).
    const model::Portfolio* spy = structure_.find_portfolio("SPY");
    const auto expect_as_priced_whole = [&](const char* what) {
        ASSERT_FALSE(snapshots_.empty()) << what;
        const model::PortfolioSnapshot& got = snapshots_.back();
        ASSERT_TRUE(got.priced) << what;
        model::PortfolioSnapshot whole = got;
        whole.priced = false;
        spy->price_snapshot(whole);
        EXPECT_EQ(copy(got.forward), copy(whole.forward)) << what;
        EXPECT_EQ(copy(got.iv), copy(whole.iv)) << what;
        expect_close(got.delta, whole.delta, what); // rescaled after a move of the underlying
        expect_close(got.gamma, whole.gamma, what);
        EXPECT_EQ(copy(got.theta), copy(whole.theta)) << what;
        EXPECT_EQ(copy(got.vega), copy(whole.vega)) << what;
        EXPECT_TRUE(std::ranges::any_of(got.iv, [](double v) { return v > 0.0; })) << what;
    };

    price(*md, under, 504.9, 505.1);
    for (std::size_t i = 1; i < all.size(); ++i) {
        quote_option(all[i], 505.0, 0.0, kQuoted);
    }
    md->publish();
    expect_as_priced_whole("the first: all priced");

    quote_option(all[7], 505.0, 0.2, kQuoted); // a few options move
    quote_option(all[40], 505.0, -0.1, kQuoted);
    md->publish();
    expect_as_priced_whole("a few quotes moved");

    price(*md, under, 505.4, 505.6); // the underlying alone
    md->publish();
    expect_as_priced_whole("the underlying moved");

    // Three seconds: within the live time step, the expiries' times stay as priced.
    const std::vector<double> years = copy(snapshots_.back().expiry_years);
    quote_option(all[3], 505.5, 0.0, kQuoted + 3s);
    md->publish();
    EXPECT_EQ(copy(snapshots_.back().expiry_years), years);

    quote_option(all[3], 505.5, 0.1, kQuoted + 11s); // past it: priced again
    md->publish();
    expect_as_priced_whole("time moved");
}

struct FakeMarketData final : core::MarketData {
    void start(MarketDataEngine& /*engine*/) override { ++starts; }
    void stop() override { ++stops; }
    int starts = 0;
    int stops = 0;
};

TEST_F(MarketDataEngineTest, WithoutASourceItCannotStart) {
    auto md = book();
    EXPECT_THROW(md->start(), std::runtime_error);
    EXPECT_FALSE(md->running());
}

TEST_F(MarketDataEngineTest, HeldAndOrderedOptionsAreOneSortedSetOfOptionsOnly) {
    auto md = book();
    md->set_held_options(std::vector<std::string>{"SPY-20991218-C-510", "SPY-20991218-C-500"});
    model::OrderRequest single;
    single.symbol = "SPY-20991218-C-500"; // held already: once
    single.legs = {{.ratio = 1, .symbol = single.symbol}};
    md->add_ordered_options(single);
    model::OrderRequest spread;
    spread.symbol = "SPREAD_SPY";
    spread.legs = {{.ratio = 1, .symbol = "SPY-20991219-P-495"},
                   {.ratio = 1, .symbol = "SPY-20991218-C-505"}};
    md->add_ordered_options(spread);
    model::OrderRequest shares;
    shares.symbol = "SPY";
    shares.legs = {{.ratio = 1, .symbol = "SPY"}};
    md->add_ordered_options(shares);
    EXPECT_EQ(md->held_and_ordered_options(),
              (std::vector<std::string>{"SPY-20991218-C-500", "SPY-20991218-C-505",
                                        "SPY-20991218-C-510", "SPY-20991219-P-495"}));

    md->clear_ordered_options();
    EXPECT_EQ(md->held_and_ordered_options(),
              (std::vector<std::string>{"SPY-20991218-C-500", "SPY-20991218-C-510"}));
    md->set_held_options({});
    EXPECT_TRUE(md->held_and_ordered_options().empty());
}

TEST_F(MarketDataEngineTest, SubscriptionsAreKeptByStrategyAndOnlyOptionsCountAsOrdered) {
    MarketDataEngine md(nullptr, structure_, nullptr, {});
    using Subscribed = std::unordered_map<std::string, std::vector<std::string>>;
    EXPECT_TRUE(md.subscribed_chains().empty());

    md.subscribe_chains("s1", std::vector{kChain});
    md.subscribe_chains("s2", std::vector{kChain, kOtherChain});
    EXPECT_EQ(md.subscribed_chains(),
              (Subscribed{{"s1", {kChain}}, {"s2", {kChain, kOtherChain}}}));

    md.set_held_options(std::vector<std::string>{"SPY-20991218-C-500"});
    model::OrderRequest call;
    call.symbol = "SPY-20991218-C-505";
    call.legs = {{.ratio = 1, .symbol = call.symbol}};
    md.add_ordered_options(call);
    md.add_ordered_options(call); // ordered already
    model::OrderRequest shares;
    shares.symbol = "SPY";
    shares.legs = {{.ratio = 1, .symbol = "SPY"}};
    md.add_ordered_options(shares); // not an option
    EXPECT_EQ(md.held_and_ordered_options(),
              (std::vector<std::string>{"SPY-20991218-C-500", "SPY-20991218-C-505"}));
    md.clear_ordered_options();
    EXPECT_EQ(md.held_and_ordered_options(), std::vector<std::string>{"SPY-20991218-C-500"});

    md.unsubscribe_chains("s2");
    EXPECT_EQ(md.subscribed_chains(), (Subscribed{{"s1", {kChain}}}));
    md.unsubscribe_chains("s1");
    EXPECT_TRUE(md.subscribed_chains().empty());
}

TEST_F(MarketDataEngineTest, StartsAndStopsTheFeedAndForgetsItsQuotes) {
    auto market_data = std::make_unique<FakeMarketData>();
    const FakeMarketData& f = *market_data;
    MarketDataEngine md(nullptr, structure_, std::move(market_data), {});
    md.start();
    md.start();
    EXPECT_EQ(f.starts, 1);
    EXPECT_TRUE(md.running());

    md.subscribe_chains("s1", std::vector{kChain});
    MarketDataEngine& book = md;
    price(book, book.instruments(21)[0], 504.9, 505.1);
    EXPECT_EQ(book.instruments(21).size(), 21U);
    md.stop();
    EXPECT_EQ(f.stops, 1);
    EXPECT_FALSE(md.running());
    EXPECT_EQ(book.instruments(21).size(), 1U); // the underlying's price is gone with the stop
}

TEST_F(MarketDataEngineTest, ANewDaysContractsAreLaidOutWithWhatWasSubscribedAndHeld) {
    MarketDataEngine md(nullptr, structure_, std::make_unique<FakeMarketData>(), {});
    md.subscribe_chains("s1", std::vector{kChain});
    md.subscribe_chains("s2", std::vector{kOtherChain});
    md.set_held_options(std::vector<std::string>{"SPY-20991218-C-505"});
    md.start();
    EXPECT_THROW(md.load_contracts(), std::logic_error); // not while the source streams
    md.stop();

    // The next day: the 12-18 expiry gone but the call held, a 12-20 expiry listed.
    std::vector<model::Contract> day{underlying()};
    for (const year_month_day expiry : {2099y / December / 19, 2099y / December / 20}) {
        for (int k = kLowStrike; k <= kHighStrike; ++k) {
            for (const auto type : {model::OptionType::CALL, model::OptionType::PUT}) {
                day.push_back(option(expiry, type, k));
            }
        }
    }
    structure_.replace_contracts("SPY", day, {"SPY-20991218-C-505"});
    md.load_contracts();

    // s1's chain now holds the held call alone; s2's is as it was; the new one can be asked for.
    MarketDataEngine& book = md;
    std::vector<Instrument> streamed = book.instruments(0);
    EXPECT_EQ(streamed.size(), 1 + 1 + 2 * kStrikes);
    EXPECT_TRUE(std::ranges::any_of(streamed, [](const Instrument& i) {
        return i.contract->symbol == "SPY-20991218-C-505";
    }));
    md.subscribe_chains("s1", std::vector<std::string>{"SPY_20991220"});
    streamed = book.instruments(0);
    EXPECT_EQ(streamed.size(), 1 + 1 + 4 * kStrikes);
}

TEST_F(MarketDataEngineTest, ASizeAloneOrARepeatedPriceSendsNoSnapshot) {
    auto md = book();
    md->subscribe_chains("s1", std::vector{kChain});
    const std::vector<Instrument> all = md->instruments(0);
    const auto call = std::ranges::find_if(
        all, [](const Instrument& i) { return i.contract->symbol == "SPY-20991218-C-505"; });
    ASSERT_NE(call, all.end());
    md->quote(call->slot, QuoteField::BidSize, 50.0, kQuoted);
    md->quote(call->slot, QuoteField::AskSize, 70.0, kQuoted);
    md->publish();
    EXPECT_TRUE(snapshots_.empty());

    md->quote(call->slot, QuoteField::Bid, 1.0, kQuoted);
    md->publish();
    ASSERT_EQ(snapshots_.size(), 1U);
    const auto column = static_cast<std::size_t>(call->slot.option);
    EXPECT_DOUBLE_EQ(snapshots_[0].bid_size[column], 50.0); // the sizes come with the next one
    EXPECT_DOUBLE_EQ(snapshots_[0].ask_size[column], 70.0);

    md->quote(call->slot, QuoteField::Bid, 1.0, kQuoted + 1s); // the same price again
    md->publish();
    EXPECT_EQ(snapshots_.size(), 1U);
}

TEST_F(MarketDataEngineTest, AWindowCentersOnTheNearestStrikeWithinTheChain) {
    // One line left for the chain still streams one strike.
    const auto window = [&](double bid, double ask, std::size_t lines) {
        auto md = book();
        md->subscribe_chains("s1", std::vector{kChain});
        price(*md, md->instruments(lines)[0], bid, ask);
        return md->instruments(lines);
    };
    EXPECT_EQ(call_strikes(window(504.9, 505.1, 1)), strike_range(505, 505));
    EXPECT_EQ(window(504.9, 505.1, 1).size(), 3U);
    EXPECT_EQ(call_strikes(window(504.2, 504.4, 3)), strike_range(504, 504)); // nearer 504
    EXPECT_EQ(call_strikes(window(599.9, 600.1, 3)), strike_range(520, 520)); // above them all
    EXPECT_EQ(call_strikes(window(479.9, 480.1, 3)), strike_range(490, 490)); // below
    EXPECT_EQ(window(504.9, 505.1, 1000).size(), 1 + 2 * kStrikes); // more lines than strikes
}

struct Recorder final : utilities::Logger {
    void log(const utilities::LogRecord& record) override { records.push_back(record); }
    std::vector<utilities::LogRecord> records;
};

TEST_F(MarketDataEngineTest, AChainOrAnOptionWithoutAContractIsNotStreamed) {
    Recorder logger;
    MarketDataEngine md(&logger, structure_, std::make_unique<FakeMarketData>(), {});
    md.subscribe_chains("s1", std::vector<std::string>{"SPY_20991230"});
    EXPECT_TRUE(std::ranges::any_of(logger.records, [](const utilities::LogRecord& r) {
        return r.level == utilities::WARNING &&
               r.msg.find("no chain SPY_20991230") != std::string::npos;
    }));
    EXPECT_TRUE(md.instruments(0).empty());
    EXPECT_EQ(md.subscribed_chains().at("s1"), std::vector<std::string>{"SPY_20991230"});

    md.set_held_options(std::vector<std::string>{"SPY-20991230-C-500"});
    EXPECT_EQ(md.held_and_ordered_options(), std::vector<std::string>{"SPY-20991230-C-500"});
    EXPECT_TRUE(md.instruments(0).empty());
}

// ── The live engine

class ZeroDteProbe final : public strategy::Strategy {
  public:
    using Strategy::Strategy;

    void on_init() override {
        const std::vector<std::string> chains = portfolio()->chains_by_expiry(0, 0);
        if (chains.empty()) {
            set_error("No 0DTE chain");
            return;
        }
        subscribe_chains(chains);
    }
    void on_stop() override {}
    void on_timer() override {}
};

REGISTER_STRATEGY(ZeroDteProbe);

TEST(LiveMarketData, AStrategyStreamsTheZeroDteChainItFindsAtInit) {
    const std::string path = ::testing::TempDir() + "optra_live_market_data.db";
    const auto remove_files = [&path] {
        for (const char* suffix : {"", "-wal", "-shm"}) {
            std::filesystem::remove(path + suffix);
        }
    };
    remove_files();
    // Expiring today (the UTC date, which the expired-contract cleanup goes by): a 0DTE chain.
    const year_month_day today{floor<days>(system_clock::now())};
    {
        adapters::SqliteDatabase db(nullptr, path);
        std::vector<model::Contract> contracts{underlying()};
        for (const double strike : {500.0, 501.0}) {
            contracts.push_back(option(today, model::OptionType::CALL, strike));
            contracts.push_back(option(today, model::OptionType::PUT, strike));
        }
        db.save_contracts(contracts);
        db.close();
    }
    ::setenv("OPTRA_DB", path.c_str(), 1);
    {
        const auto made = live::make_main(
            [](utilities::Logger* logger, const model::Portfolios&) {
                return live::make_gateway(logger);
            },
            std::make_unique<FakeMarketData>(), [] { return system_clock::now(); });
        core::Main& main = *made;
        core::StrategyEngine* se = main.strategy_engine();
        const std::string name =
            se->add_strategy({.class_name = "ZeroDteProbe", .portfolio = "SPY"});
        se->init_strategy(name); // before any snapshot: no quote has come yet
        EXPECT_TRUE(se->error(name).empty());
        EXPECT_EQ(main.market_data()->instruments(0).size(),
                  5U); // the underlying, 2 calls, 2 puts

        main.remove_strategy(name); // its chains dropped with it
        EXPECT_TRUE(main.market_data()->instruments(0).empty());
        main.close();
    }
    ::unsetenv("OPTRA_DB");
    remove_files();
}

} // namespace
