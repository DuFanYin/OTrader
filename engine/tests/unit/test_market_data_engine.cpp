/**
 * Market data. The quote book: the chains strategies subscribe become the instruments a feed
 * streams (all of them, or a window around the money that fits the feed's lines), and quotes
 * become snapshots in the portfolio's column order after each batch, priced (only what moved
 * priced again, to the same values as pricing all). The engine starts and stops
 * its feed. In the live engine a strategy finds its 0DTE chain at init, before any quote has
 * arrived, and what it subscribes is what gets streamed.
 */
#include "core/engines/option_strategy.hpp"
#include "model/portfolio_structure.hpp"
#include "adapters/db/engine_db_sqlite.hpp"
#include "adapters/marketdata/engine_market_data.hpp"
#include "adapters/marketdata/quote_book.hpp"
#include "core/engine_live.hpp"
#include "strategy/strategy_registry.hpp"
#include "strategy/template.hpp"
#include "model/symbol.hpp"
#include "model/utility.hpp"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <gtest/gtest.h>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace std::chrono;
using adapters::Instrument;
using adapters::MarketDataEngine;
using adapters::QuoteBook;
using adapters::QuoteField;

const std::string kChain = "SPY_20991218";
// When the test's quotes were taken: a snapshot is as of its latest one.
const model::DateTime kQuoted = sys_days{2099y / December / 18} + 15h;
const std::string kOtherChain = "SPY_20991219";
constexpr int kLowStrike = 490;
constexpr int kHighStrike = 520;
constexpr std::size_t kStrikes = kHighStrike - kLowStrike + 1;

auto underlying() -> model::ContractData {
    model::ContractData c;
    c.symbol = "SPY";
    c.product = model::Product::EQUITY;
    c.trading_class = "SPY";
    c.con_id = 756733;
    return c;
}

auto option(year_month_day expiry, model::OptionType type, double strike)
    -> model::ContractData {
    const bool call = type == model::OptionType::CALL;
    model::ContractData c;
    c.symbol =
        model::option_symbol({.root = "SPY", .expiry = expiry, .type = type, .strike = strike});
    c.product = model::Product::OPTION;
    c.size = 100;
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

/** SPY with two chains of calls and puts at every strike from 490 to 520. */
class QuoteBookTest : public ::testing::Test {
  protected:
    void SetUp() override {
        structure_.ensure_portfolio("SPY");
        structure_.process_underlying_for_portfolio("SPY", underlying());
        for (const year_month_day expiry : {2099y / December / 18, 2099y / December / 19}) {
            for (int k = kLowStrike; k <= kHighStrike; ++k) {
                for (const auto type : {model::OptionType::CALL, model::OptionType::PUT}) {
                    structure_.process_option_for_portfolio("SPY", option(expiry, type, k));
                }
            }
        }
        structure_.finalize_all_chains();
    }

    auto book(milliseconds interval = 1s) -> std::unique_ptr<QuoteBook> {
        return std::make_unique<QuoteBook>(
            structure_, [this](const model::PortfolioSnapshot& s) { snapshots_.push_back(s); },
            interval);
    }

    /** Quote the underlying at bid / ask. */
    static void price(QuoteBook& book, const Instrument& under, double bid, double ask) {
        book.quote(under.slot, QuoteField::Bid, bid, kQuoted);
        book.quote(under.slot, QuoteField::Ask, ask, kQuoted);
    }

    model::PortfolioStructure structure_;
    std::vector<model::PortfolioSnapshot> snapshots_;
};

/** A chain big enough that pricing runs on the shared pool: priced again after the underlying
 * alone moved (each option's own flags read on the pool's threads), it is what pricing it all
 * gives. */
TEST(PortfolioPricing, BigChainRepricedOnPoolAfterUnderlyingMove) {
    model::PortfolioStructure structure;
    structure.ensure_portfolio("SPY");
    structure.process_underlying_for_portfolio("SPY", underlying());
    for (const year_month_day expiry : {2099y / December / 18, 2099y / December / 19}) {
        for (int k = 400; k < 600; ++k) {
            for (const auto type : {model::OptionType::CALL, model::OptionType::PUT}) {
                structure.process_option_for_portfolio("SPY", option(expiry, type, k));
            }
        }
    }
    structure.finalize_all_chains();
    const model::PortfolioData* spy = structure.get_portfolio("SPY");
    const auto& order = spy->option_apply_order();
    ASSERT_GT(order.size(), 256U); // above the pool's inline threshold

    model::PortfolioSnapshot s;
    s.portfolio_name = "SPY";
    s.datetime = kQuoted;
    s.underlying_bid = 499.9;
    s.underlying_ask = 500.1;
    for (const auto* o : order) {
        const double k = o->strike;
        const bool call = o->type == model::OptionType::CALL;
        const double mid = std::max(call ? 500.0 - k : k - 500.0, 0.0) + 1.5;
        s.bid.push_back(mid - 0.05);
        s.ask.push_back(mid + 0.05);
        s.mark.push_back(mid);
    }
    spy->price_snapshot(s);
    ASSERT_TRUE(s.priced);

    s.underlying_bid = 501.9; // the underlying alone moves
    s.underlying_ask = 502.1;
    const std::vector<std::uint8_t> unchanged(order.size(), 0);
    spy->price_snapshot(s, unchanged, true);
    model::PortfolioSnapshot whole = s;
    whole.priced = false;
    spy->price_snapshot(whole);
    EXPECT_EQ(s.iv, whole.iv);
    EXPECT_EQ(s.delta, whole.delta);
    EXPECT_EQ(s.forward, whole.forward);
}

/** SPXW: options on SPX, an index that does not trade, hedged with SPY. */
TEST(QuoteBookHedge, AnIndexPortfolioAlsoStreamsWhatItHedgesWith) {
    model::PortfolioStructure structure;
    structure.ensure_portfolios_created(); // SPY and SPXW (core/portfolios.hpp)
    model::ContractData spx;
    spx.symbol = "SPX";
    spx.product = model::Product::INDEX;
    spx.con_id = 416904;
    structure.process_underlying(spx);
    structure.process_underlying(underlying()); // SPY: the SPY portfolio's, and SPXW's hedge
    model::ContractData call = option(2099y / December / 18, model::OptionType::CALL, 6000);
    call.symbol = model::option_symbol({.root = "SPXW",
                                            .expiry = 2099y / December / 18,
                                            .type = model::OptionType::CALL,
                                            .strike = 6000});
    call.trading_class = "SPXW";
    call.option_underlying = "SPX";
    structure.process_option(call);
    structure.finalize_all_chains();

    std::vector<model::PortfolioSnapshot> snapshots;
    QuoteBook book(
        structure, [&](const model::PortfolioSnapshot& s) { snapshots.push_back(s); }, 1s);
    book.subscribe("s", std::vector<std::string>{"SPXW_20991218"});
    const std::vector<Instrument> streamed = book.instruments(0);
    const auto slot_of = [&](const std::string& symbol) {
        const auto it = std::ranges::find(streamed, symbol,
                                          [](const Instrument& i) { return i.contract->symbol; });
        EXPECT_NE(it, streamed.end()) << symbol;
        return it != streamed.end() ? it->slot : adapters::QuoteSlot{};
    };
    EXPECT_EQ(slot_of("SPX").option, adapters::QuoteSlot::kUnderlying);
    const adapters::QuoteSlot spy = slot_of("SPY");
    EXPECT_EQ(spy.option, adapters::QuoteSlot::kHedge);
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
    model::PortfolioData* spxw = structure.get_portfolio("SPXW");
    spxw->apply_frame(snapshots[0]);
    ASSERT_NE(spxw->hedge(), nullptr);
    EXPECT_EQ(spxw->hedge()->symbol, "SPY");
    EXPECT_DOUBLE_EQ(spxw->hedge()->theo_delta, 0.1);
}

TEST_F(QuoteBookTest, StreamsTheSubscribedChainsAndTheirUnderlying) {
    auto md = book();
    EXPECT_TRUE(md->instruments(0).empty());

    md->subscribe("s1", std::vector{kChain});
    const std::vector<Instrument> one = md->instruments(0);
    ASSERT_EQ(one.size(), 1 + 2 * kStrikes);
    EXPECT_EQ(one[0].contract->symbol, "SPY");
    EXPECT_LT(one[0].slot.option, 0);
    EXPECT_TRUE(std::ranges::all_of(one.begin() + 1, one.end(), [](const Instrument& i) {
        return i.contract->symbol.starts_with("SPY-20991218-") && i.slot.option >= 0;
    }));

    md->subscribe("s2", std::vector{kChain, kOtherChain}); // kChain once, not twice
    EXPECT_EQ(md->instruments(0).size(), 1 + 4 * kStrikes);
    md->unsubscribe("s2");
    EXPECT_EQ(md->instruments(0).size(), 1 + 2 * kStrikes);
    md->unsubscribe("s1");
    EXPECT_TRUE(md->instruments(0).empty());
}

TEST_F(QuoteBookTest, AWindowAroundTheMoneyFitsTheLines) {
    auto md = book();
    md->subscribe("s1", std::vector{kChain});
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
    md->subscribe("s2", std::vector{kOtherChain});
    EXPECT_EQ(md->instruments(21).size(), 1U + 2 * 2 * 5);
}

TEST_F(QuoteBookTest, QuotesBecomeSnapshotsInColumnOrderAsTheyChange) {
    auto md = book(1s);
    md->subscribe("s1", std::vector{kChain});
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
    const auto& order = structure_.get_portfolio("SPY")->option_apply_order();
    ASSERT_EQ(s.bid.size(), order.size());
    const auto column = static_cast<std::size_t>(call->slot.option);
    EXPECT_EQ(order[column]->symbol, call->contract->symbol);
    EXPECT_DOUBLE_EQ(s.bid[column], 1.0);
    EXPECT_DOUBLE_EQ(s.ask[column], 1.2);
    EXPECT_DOUBLE_EQ(s.mark[column], 1.1); // marked at the mid, as the backtest marks it
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

TEST_F(QuoteBookTest, OnlyWhatMovedIsPricedAgainToTheSameValues) {
    auto md = book();
    md->subscribe("s1", std::vector{kChain, kOtherChain});
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
    // Each snapshot as priced is what pricing it all from its quotes gives, bit for bit.
    const model::PortfolioData* spy = structure_.get_portfolio("SPY");
    const auto expect_as_priced_whole = [&](const char* what) {
        ASSERT_FALSE(snapshots_.empty()) << what;
        const model::PortfolioSnapshot& got = snapshots_.back();
        ASSERT_TRUE(got.priced) << what;
        model::PortfolioSnapshot whole = got;
        whole.priced = false;
        spy->price_snapshot(whole);
        EXPECT_EQ(got.forward, whole.forward) << what;
        EXPECT_EQ(got.iv, whole.iv) << what;
        EXPECT_EQ(got.delta, whole.delta) << what;
        EXPECT_EQ(got.gamma, whole.gamma) << what;
        EXPECT_EQ(got.theta, whole.theta) << what;
        EXPECT_EQ(got.vega, whole.vega) << what;
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

    quote_option(all[3], 505.5, 0.0, kQuoted + 3s); // time moved
    md->publish();
    expect_as_priced_whole("time moved");
}

struct FakeFeed final : adapters::MarketDataFeed {
    void start(QuoteBook& /*book*/, const utilities::BaseEngine& /*log*/) override { ++starts; }
    void stop() override { ++stops; }
    int starts = 0;
    int stops = 0;
};

class MarketDataEngineTest : public QuoteBookTest {};

TEST_F(MarketDataEngineTest, StartsAndStopsTheFeedAndForgetsItsQuotes) {
    MarketDataEngine without_source(nullptr, structure_, nullptr, {});
    EXPECT_THROW(without_source.start(), std::runtime_error); // a build without one

    auto feed = std::make_unique<FakeFeed>();
    const FakeFeed& f = *feed;
    MarketDataEngine md(nullptr, structure_, std::move(feed), {});
    md.start();
    md.start();
    EXPECT_EQ(f.starts, 1);
    EXPECT_TRUE(md.running());

    md.subscribe_chains("s1", std::vector{kChain});
    QuoteBook& book = md.book();
    price(book, book.instruments(21)[0], 504.9, 505.1);
    EXPECT_EQ(book.instruments(21).size(), 21U);
    md.stop();
    EXPECT_EQ(f.stops, 1);
    EXPECT_FALSE(md.running());
    EXPECT_EQ(book.instruments(21).size(), 1U); // the underlying's price is gone with the stop
}

TEST_F(MarketDataEngineTest, RecordsEachPortfolioAsItStoodAtTheEndOfEachMinute) {
    const std::filesystem::path dir = ::testing::TempDir() + "optra_recorded";
    std::filesystem::remove_all(dir);
    {
        MarketDataEngine md(nullptr, structure_, std::make_unique<FakeFeed>(), {});
        md.record_to(dir.string());
        md.start();
        md.subscribe_chains("s1", std::vector{kChain});
        QuoteBook& book = md.book(); // the feed's thread's: this one's while the fake streams
        const std::vector<Instrument> all = book.instruments(0);
        const auto call = std::ranges::find_if(all, [](const Instrument& i) {
            return i.contract->symbol == "SPY-20991218-C-505";
        });
        ASSERT_NE(call, all.end());
        const auto quote = [&](double bid, model::DateTime at) {
            book.quote(all[0].slot, QuoteField::Bid, 504.9, at);
            book.quote(all[0].slot, QuoteField::Ask, 505.1, at);
            book.quote(call->slot, QuoteField::Bid, bid, at);
            book.quote(call->slot, QuoteField::Ask, bid + 0.2, at);
            book.publish();
        };
        quote(1.0, kQuoted + 10s);
        quote(1.1, kQuoted + 50s); // the same minute: the later quote is the minute's
        quote(1.3, kQuoted + 70s); // the next minute
        md.stop();                 // the last minute is written too
    }
    const std::filesystem::path file = dir / "SPY" / "SPY-2099-12" / "20991218.csv";
    ASSERT_TRUE(std::filesystem::exists(file));
    std::ifstream in(file);
    std::vector<std::string> lines;
    for (std::string line; std::getline(in, line);) {
        lines.push_back(line);
    }
    ASSERT_EQ(lines.size(), 3U); // the header, a row for each minute
    EXPECT_EQ(lines[0], "ts_ns,symbol,bid,ask,underlying_bid,underlying_ask");
    const auto ns = [](model::DateTime t) { return std::to_string(duration_cast<nanoseconds>(t.time_since_epoch()).count()); };
    EXPECT_EQ(lines[1], ns(kQuoted + 1min) + ",991218C00505000,1.1,1.3,504.9,505.1");
    EXPECT_EQ(lines[2], ns(kQuoted + 2min) + ",991218C00505000,1.3,1.5,504.9,505.1");
    std::filesystem::remove_all(dir);
}

// ── The live engine

class ZeroDteProbe final : public strategy::OptionStrategyTemplate {
  public:
    using OptionStrategyTemplate::OptionStrategyTemplate;

    void on_init_logic() override {
        const std::vector<std::string> chains = portfolio()->get_chain_by_expiry(0, 0);
        if (chains.empty()) {
            set_error("No 0DTE chain");
            return;
        }
        subscribe_chains(chains);
    }
    void on_stop_logic() override {}
    void on_timer_logic() override {}
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
        adapters::DatabaseEngine db(nullptr, path);
        std::vector<model::ContractData> contracts{underlying()};
        for (const double strike : {500.0, 501.0}) {
            contracts.push_back(option(today, model::OptionType::CALL, strike));
            contracts.push_back(option(today, model::OptionType::PUT, strike));
        }
        db.save_contracts(contracts);
        db.close();
    }
    ::setenv("OPTRA_DB", path.c_str(), 1);
    {
        core::live::MainEngine main;
        core::OptionStrategyEngine* se = main.option_strategy_engine();
        const std::string name = se->add_strategy("ZeroDteProbe", "SPY");
        se->init_strategy(name); // before any snapshot: no quote has come yet
        EXPECT_FALSE(se->get_strategy(name)->error());
        EXPECT_EQ(main.market_data()->book().instruments(0).size(),
                  5U); // the underlying, 2 calls, 2 puts

        se->remove_strategy(name);
        EXPECT_TRUE(main.market_data()->book().instruments(0).empty());
        main.close();
    }
    ::unsetenv("OPTRA_DB");
    remove_files();
}

} // namespace
