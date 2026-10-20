/**
 * A portfolio's listing and frames (Portfolio), and the structure that holds the portfolios
 * (Portfolios): the columns' order; a snapshot's views after a copy or a move; a rate curve; a
 * frame of another listing moves only the clock and the underlying; a frame read in place or
 * copied; a roll keeps what stays; the at-the-money strike; chains by expiry and a day's options
 * in place of the ones before; contracts of no portfolio; a hedge named by a portfolio's spec; a
 * leg's rule picking its chain and its option.
 */
#include "model/portfolio.hpp"
#include "model/session_calendar.hpp"
#include "model/portfolios.hpp"
#include "model/portfolio_spec.hpp"
#include "model/symbol.hpp"
#include "model/utility.hpp"
#include "pricing/pricing.hpp"
#include "quant/engine_api.h"
#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <gtest/gtest.h>
#include <initializer_list>
#include <map>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono;
using model::OptionType;

const year_month_day kExpiry = 2099y / December / 18;
// 10:00 New York, a few weeks before the expiry.
const model::DateTime kQuoted = sys_days{2099y / December / 1} + 15h;

auto symbol(OptionType type, double strike, year_month_day expiry = kExpiry) -> std::string {
    return model::option_symbol({.root = "SPY", .expiry = expiry, .type = type, .strike = strike});
}

auto contract(const std::string& symbol, double size) -> model::Contract {
    model::Contract c;
    c.symbol = symbol;
    c.multiplier = size;
    return c;
}

auto contracts(std::initializer_list<std::string> options) -> std::vector<model::Contract> {
    std::vector<model::Contract> out;
    for (const std::string& o : options) {
        out.push_back(contract(o, 100.0));
    }
    return out;
}

/** SPY listing these options (and its underlying, unless bare). */
auto portfolio(std::initializer_list<std::string> options, bool bare = false)
    -> std::unique_ptr<model::Portfolio> {
    auto p = std::make_unique<model::Portfolio>("SPY");
    if (!bare) {
        p->set_underlying(contract("SPY", 1.0));
    }
    p->list(std::make_shared<const model::Listing>(contracts(options)));
    return p;
}

/** A snapshot at kQuoted with the underlying at spot and each option at (bid, ask), by symbol;
 * an option not given is not quoted. */
auto snapshot(const model::Portfolio& p, double spot,
              const std::map<std::string, std::pair<double, double>>& quotes)
    -> model::PortfolioSnapshot {
    model::PortfolioSnapshot s = p.snapshot();
    s.datetime = kQuoted;
    s.underlying_bid = spot > 0.0 ? spot - 0.1 : 0.0;
    s.underlying_ask = spot > 0.0 ? spot + 0.1 : 0.0;
    s.underlying_mark = spot;
    for (const auto& [option, quote] : quotes) {
        const std::uint32_t column = *p.listing().column(option);
        s.values(&model::Frame::bid)[column] = quote.first;
        s.values(&model::Frame::ask)[column] = quote.second;
    }
    return s;
}

/** Priced as live prices it, before it is applied. */
void price(model::PortfolioSnapshot& s) { pricing::price_snapshot(s, model::RateCurve{}, "mid"); }

TEST(PortfolioListing, ColumnsRunByChainThenBySymbolWithEachChainsStrikes) {
    const std::string c610 = symbol(OptionType::CALL, 610);
    const std::string p600 = symbol(OptionType::PUT, 600);
    const std::string c600 = symbol(OptionType::CALL, 600);
    const std::string later = symbol(OptionType::CALL, 600, 2099y / December / 19);
    const std::vector<model::Contract> given{contract(later, 100.0), contract(c610, 100.0),
                                                 contract(p600, 100.0), contract(c600, 50.0),
                                                 contract(c600, 100.0), contract("SPY", 1.0)};
    const model::Listing listing(given);
    ASSERT_EQ(listing.size(), 4U); // the underlying left out, c600 once
    EXPECT_EQ(listing.contract(0).symbol, c600);
    EXPECT_EQ(listing.contract(0).multiplier, 100.0); // the last given
    EXPECT_EQ(listing.contract(1).symbol, c610);
    EXPECT_EQ(listing.contract(2).symbol, p600);
    EXPECT_EQ(listing.contract(3).symbol, later);
    EXPECT_EQ(listing.column(p600), 2U);
    EXPECT_FALSE(listing.column("SPY"));

    ASSERT_EQ(listing.chains().size(), 2U);
    const model::Listing::Chain& chain = listing.chains()[0];
    EXPECT_EQ(chain.symbol, "SPY_20991218");
    EXPECT_EQ(chain.strikes, (std::vector<double>{600, 610}));
    EXPECT_EQ(chain.calls, (std::vector<std::int32_t>{0, 1}));
    EXPECT_EQ(chain.puts, (std::vector<std::int32_t>{2, -1}));
    EXPECT_EQ(listing.chain_of(3), 1U);
    EXPECT_EQ(listing.expiries().size(), 2U);
    EXPECT_EQ(listing.chains()[1].expiry_index, 1U);
}

TEST(PortfolioSnapshot, ACopyOrAMoveViewsItsOwnValues) {
    const std::string call = symbol(OptionType::CALL, 600);
    auto p = portfolio({call});
    model::PortfolioSnapshot s = snapshot(*p, 600.0, {{call, {10.0, 12.0}}});
    model::PortfolioSnapshot copy = s;
    s.values(&model::Frame::bid)[0] = 1.0;
    EXPECT_EQ(copy.bid[0], 10.0);
    EXPECT_EQ(copy.portfolio_name, "SPY");
    EXPECT_EQ(copy.listing, &p->listing());

    model::PortfolioSnapshot moved = std::move(copy);
    EXPECT_EQ(moved.bid[0], 10.0);
    EXPECT_EQ(moved.forward.size(), 1U);
    EXPECT_EQ(moved.expiry_years.size(), 1U);
    copy = moved;
    EXPECT_EQ(copy.ask[0], 12.0);
    EXPECT_NE(copy.ask.data(), moved.ask.data());
}

TEST(PortfolioPricing, ARateCurveIsStraightBetweenItsPointsAndFlatBeyondThem) {
    const model::RateCurve c{.years = {1.0 / 12, 1.0}, .rates = {0.01, 0.04}};
    EXPECT_EQ(c.at(0.0), 0.01);
    EXPECT_EQ(c.at(1.0 / 12), 0.01);
    EXPECT_DOUBLE_EQ(c.at(0.5), 0.01 + ((0.5 - (1.0 / 12)) / (1.0 - (1.0 / 12)) * 0.03));
    EXPECT_EQ(c.at(2.0), 0.04);
    EXPECT_EQ(model::RateCurve::flat(0.05).at(3.0), 0.05);
}

TEST(PortfolioFrames, AFrameOfAnotherListingMovesOnlyTheClockAndTheUnderlying) {
    const std::string call = symbol(OptionType::CALL, 600);
    auto p = portfolio({call});
    model::PortfolioSnapshot s = snapshot(*p, 600.0, {{call, {1.0, 1.2}}});
    price(s);
    p->apply(s);
    auto other = portfolio({call});
    model::PortfolioSnapshot moved = snapshot(*other, 601.0, {{call, {9.0, 9.2}}});
    moved.datetime = kQuoted + 1min;
    p->apply(moved);
    EXPECT_EQ(p->datetime(), kQuoted + 1min);
    EXPECT_DOUBLE_EQ(p->underlying()->mid(), 601.0);
    EXPECT_DOUBLE_EQ(p->option(call)->bid(), 1.0);
}

TEST(PortfolioFrames, ASnapshotIsTakenOverAndAFrameReadInPlaceWhileItsStorageKeepsIt) {
    const std::string call = symbol(OptionType::CALL, 600);
    auto p = portfolio({call});
    model::PortfolioSnapshot s = snapshot(*p, 600.0, {{call, {10.0, 12.0}}});
    s.values(&model::Frame::bid_size)[0] = 5.0;
    price(s);
    p->apply(s); // taken over: s has the zeros it had
    EXPECT_TRUE(p->frame().priced);
    EXPECT_DOUBLE_EQ(s.bid[0], 0.0);
    const model::Option option = *p->option(call);
    EXPECT_DOUBLE_EQ(option.bid(), 10.0);
    EXPECT_DOUBLE_EQ(option.mid(), 11.0);
    EXPECT_DOUBLE_EQ(option.bid_size(), 5.0);
    EXPECT_GT(option.iv(), 0.0);
    EXPECT_DOUBLE_EQ(option.delta(), p->frame().delta[0] * 100.0);

    auto stored = std::make_shared<model::PortfolioSnapshot>(snapshot(*p, 600.0, {{call, {7, 8}}}));
    price(*stored);
    p->apply(*stored, stored);
    EXPECT_EQ(p->frame().bid.data(), stored->bid.data()); // read where it is
    stored->values(&model::Frame::bid)[0] = 7.5;
    EXPECT_DOUBLE_EQ(p->option(call)->bid(), 7.5);

    model::PortfolioSnapshot copied = snapshot(*p, 600.0, {{call, {3.0, 4.0}}});
    price(copied);
    p->apply(static_cast<const model::Frame&>(copied));
    copied.values(&model::Frame::bid)[0] = 0.0;
    EXPECT_DOUBLE_EQ(p->option(call)->bid(), 3.0); // a copy of its own
}

TEST(PortfolioFrames, AHedgeIsCountedOnlyWhenBothArePriced) {
    const std::string call = symbol(OptionType::CALL, 600);
    auto p = portfolio({call});
    p->set_hedge_instrument(contract("SPYX", 1.0));
    model::PortfolioSnapshot s = snapshot(*p, 600.0, {{call, {10.0, 12.0}}});
    price(s);
    p->apply(s);
    EXPECT_DOUBLE_EQ(p->hedge()->theo_delta(), 0.0); // not priced
    EXPECT_DOUBLE_EQ(p->underlying()->theo_delta(), 1.0);

    s = snapshot(*p, 600.0, {{call, {10.0, 12.0}}});
    s.hedge_bid = 299.9;
    s.hedge_ask = 300.1;
    s.hedge_mark = 300.0;
    price(s);
    p->apply(s);
    EXPECT_DOUBLE_EQ(p->hedge()->theo_delta(), 0.5);
    EXPECT_EQ(p->instrument("SPYX")->symbol(), "SPYX");
    EXPECT_FALSE(p->instrument("DIA"));

    model::PortfolioSnapshot no_spot = snapshot(*p, 0.0, {{call, {10.0, 12.0}}});
    no_spot.hedge_mark = 300.0;
    price(no_spot);
    p->apply(no_spot);
    EXPECT_DOUBLE_EQ(p->hedge()->theo_delta(), 0.0);
}

TEST(PortfolioFrames, ARollKeepsTheValuesOfWhatStays) {
    const std::string stays = symbol(OptionType::CALL, 600);
    const std::string goes = symbol(OptionType::CALL, 610);
    const std::string comes = symbol(OptionType::CALL, 600, 2099y / December / 19);
    auto p = portfolio({stays, goes});
    model::PortfolioSnapshot s = snapshot(*p, 600.0, {{stays, {10.0, 12.0}}, {goes, {5, 6}}});
    price(s);
    s.values(&model::Frame::smile)[model::kSmilePoints] = 7.0; // as a fit would leave it
    p->apply(s);
    const double forward = p->chain("SPY_20991218")->forward();
    EXPECT_GT(forward, 0.0);

    EXPECT_TRUE(p->list(std::make_shared<const model::Listing>(contracts({stays, comes}))).empty());
    EXPECT_DOUBLE_EQ(p->option(stays)->bid(), 10.0);
    EXPECT_DOUBLE_EQ(p->option(comes)->bid(), 0.0);
    EXPECT_FALSE(p->option(goes));
    EXPECT_DOUBLE_EQ(p->chain("SPY_20991218")->forward(), forward);
    EXPECT_EQ(p->chain("SPY_20991218")->smile().points, 7);
    EXPECT_EQ(p->chain("SPY_20991219")->smile().points, 0);
    EXPECT_DOUBLE_EQ(p->underlying()->mid(), 600.0);
    EXPECT_EQ(p->datetime(), kQuoted);
}

/** The underlying's open, high and low today: from the session's frames (the open's bar is minute
 * 1), anew each day. */
TEST(PortfolioFrames, TheDaysOpenHighAndLowAreTheSessionsFrames) {
    auto p = portfolio({symbol(OptionType::CALL, 600)});
    const auto at = [&](model::DateTime t, double spot) {
        model::PortfolioSnapshot s = snapshot(*p, spot, {});
        s.datetime = t;
        price(s);
        p->apply(s);
    };
    const model::DateTime open = model::kNyse.at(2099y / December / 1, 9h + 30min);
    at(open, 590.0); // 09:30: before the open's bar
    EXPECT_EQ(p->day_first_minute(), -1);
    at(open + 1min, 600.0);
    at(open + 2min, 605.0);
    at(open + 3min, 598.0);
    EXPECT_DOUBLE_EQ(p->day_open(), 600.0);
    EXPECT_DOUBLE_EQ(p->day_high(), 605.0);
    EXPECT_DOUBLE_EQ(p->day_low(), 598.0);
    EXPECT_EQ(p->day_first_minute(), 1);
    at(open + 24h + 40min, 610.0); // the next day, 40 minutes in
    EXPECT_DOUBLE_EQ(p->day_open(), 610.0);
    EXPECT_DOUBLE_EQ(p->day_low(), 610.0);
    EXPECT_EQ(p->day_first_minute(), 40);
}

/** An engine that started late takes the day's open, high and low from its source: the open's,
 * the highest and lowest of both; another day's are left out, and so is one once the open was
 * seen. */
TEST(PortfolioFrames, ADaysRangeFromTheSourceIsWhereTheDayBegan) {
    auto p = portfolio({symbol(OptionType::CALL, 600)});
    const auto at = [&](model::DateTime t, double spot) {
        model::PortfolioSnapshot s = snapshot(*p, spot, {});
        s.datetime = t;
        price(s);
        p->apply(s);
    };
    const year_month_day day = 2099y / December / 1;
    const model::DateTime open = model::kNyse.at(day, 9h + 30min);
    at(open + 40min, 600.0);
    EXPECT_FALSE(p->saw_open());
    p->restore_day(2099y / November / 30, 590.0, 610.0, 580.0); // another day's
    EXPECT_FALSE(p->saw_open());
    p->restore_day(day, 595.0, 599.0, 585.0);
    EXPECT_TRUE(p->saw_open());
    EXPECT_EQ(p->day_first_minute(), 1);
    EXPECT_DOUBLE_EQ(p->day_open(), 595.0);
    EXPECT_DOUBLE_EQ(p->day_high(), 600.0); // its own frame's
    EXPECT_DOUBLE_EQ(p->day_low(), 585.0);
    p->restore_day(day, 500.0, 700.0, 400.0); // seen now: kept
    EXPECT_DOUBLE_EQ(p->day_open(), 595.0);
    at(open + 41min, 620.0);
    EXPECT_DOUBLE_EQ(p->day_high(), 620.0);
}

TEST(PortfolioChains, TheAtTheMoneyStrikeIsTheNearestOrWithoutAPriceTheMiddleOne) {
    const std::initializer_list<std::string> calls{
        symbol(OptionType::CALL, 590), symbol(OptionType::CALL, 600),
        symbol(OptionType::CALL, 610), symbol(OptionType::CALL, 620)};
    auto bare = portfolio(calls, true);
    EXPECT_EQ(bare->chain("SPY_20991218")->atm_strike(), 610.0);

    auto p = portfolio(calls);
    const auto atm = [&](double spot) {
        model::PortfolioSnapshot s = snapshot(*p, spot, {});
        s.priced = true; // no forward: the underlying's mid
        p->apply(s);
        return p->chain("SPY_20991218")->atm_strike();
    };
    EXPECT_EQ(atm(605.0), 600.0); // of two as near, the lower
    EXPECT_EQ(atm(607.0), 610.0);
    EXPECT_FALSE(p->chain("SPY_20991218")->atm_iv());
}

TEST(PortfolioChains, ChainsByExpiryAndADaysOptionsInPlaceOfTheOnesBefore) {
    const std::string today = symbol(OptionType::CALL, 600);
    const std::string tomorrow = symbol(OptionType::CALL, 600, 2099y / December / 19);
    const std::string week = symbol(OptionType::CALL, 600, 2099y / December / 25);
    auto p = portfolio({today, tomorrow, week});
    p->set_trading_day(sys_days{kExpiry});
    EXPECT_EQ(p->chains_by_expiry(0, 0), std::vector<std::string>{"SPY_20991218"});
    EXPECT_EQ(p->chains_by_expiry(1, 7),
              (std::vector<std::string>{"SPY_20991219", "SPY_20991225"}));
    EXPECT_TRUE(p->chains_by_expiry(2, 6).empty());
    EXPECT_EQ(p->chain("SPY_20991225")->days_to_expiry(), 7);

    const std::vector<model::Contract> day{contract(tomorrow, 100.0)};
    EXPECT_EQ(p->replace_options(day, {week}), std::vector<std::string>{"SPY_20991218"});
    EXPECT_EQ(p->listing().size(), 2U); // the week's kept
    EXPECT_TRUE(p->chain("SPY_20991219"));
    EXPECT_FALSE(p->chain("SPY_20991218"));

    auto bare = portfolio({today}, true);
    EXPECT_EQ(bare->replace_options({}, {}), std::vector<std::string>{"SPY_20991218"});
    EXPECT_TRUE(bare->listing().chains().empty());
}

TEST(Portfolios, AContractOfNoPortfolioJoinsNone) {
    model::Portfolios structure;
    structure.ensure_portfolios_created();
    const std::string dia = model::option_symbol(
        {.root = "DIA", .expiry = kExpiry, .type = OptionType::CALL, .strike = 400});
    structure.load_contracts(std::vector{contract(dia, 100.0), contract("DIA", 1.0)});
    EXPECT_EQ(structure.find_contract(dia), nullptr);
    EXPECT_EQ(structure.find_contract("DIA"), nullptr);
    EXPECT_EQ(structure.portfolio_of(dia), nullptr);
    EXPECT_FALSE(structure.trades_contract(dia));
    EXPECT_TRUE(structure.replace_contracts("DIA", {}, std::unordered_set<std::string>{}).empty());

    model::Portfolio* spy = structure.find_portfolio("SPY");
    structure.ensure_portfolio("SPY"); // there already: the same
    EXPECT_EQ(structure.find_portfolio("SPY"), spy);
    EXPECT_EQ(structure.portfolio_names().size(), model::kPortfolios.size());
}

TEST(Portfolios, AnInstrumentIsItsOwnPortfoliosBeforeOneItHedges) {
    model::Portfolios structure;
    structure.ensure_portfolios_created();
    // SPY: SPY's underlying, and what SPXW hedges with.
    structure.load_contracts(std::vector{contract("SPX", 1.0), contract("SPY", 1.0)});
    EXPECT_EQ(structure.portfolio_of("SPY"), structure.find_portfolio("SPY"));
    EXPECT_EQ(structure.portfolio_of("SPX"), structure.find_portfolio("SPXW"));
    ASSERT_TRUE(structure.find_portfolio("SPXW")->hedge());
    EXPECT_EQ(structure.find_portfolio("SPXW")->hedge()->symbol(), "SPY");
    EXPECT_EQ(structure.find_contract("SPX")->symbol, "SPX");

    static constexpr model::PortfolioSpec kOnlyHedged{
        .name = "ONLY_HEDGED", .underlying = "ONLY_HEDGED", .hedge = "DIA"};
    structure.ensure_portfolio(kOnlyHedged);
    EXPECT_EQ(structure.portfolio_of("DIA"), structure.find_portfolio("ONLY_HEDGED"));
    EXPECT_TRUE(structure.trades_contract("DIA"));
    EXPECT_FALSE(structure.trades_contract("EEM"));
}

TEST(Portfolios, ADaysContractsReplaceOnlyThatPortfoliosOptions) {
    model::Portfolios structure;
    structure.ensure_portfolios_created();
    const std::string spy = symbol(OptionType::CALL, 600);
    const std::string spxw = model::option_symbol(
        {.root = "SPXW", .expiry = kExpiry, .type = OptionType::CALL, .strike = 6000});
    structure.load_contracts(
        std::vector{contract("SPY", 1.0), contract(spy, 100.0), contract(spxw, 100.0)});
    EXPECT_NE(structure.find_contract(spy), nullptr);

    const std::vector<model::Contract> day{contract("SPY", 1.0)};
    EXPECT_EQ(structure.replace_contracts("SPY", day, std::unordered_set<std::string>{}),
              std::vector<std::string>{"SPY_20991218"});
    EXPECT_EQ(structure.find_contract(spy), nullptr);
    EXPECT_NE(structure.find_contract(spxw), nullptr);
    EXPECT_NE(structure.find_contract("SPY"), nullptr);
}

TEST(PortfolioLegs, ALegsRulePicksItsChainByDaysToExpiry) {
    const std::string near = symbol(OptionType::CALL, 600, 2099y / December / 4);
    const std::string far = symbol(OptionType::CALL, 600, 2099y / December / 18);
    auto p = portfolio({near, far});
    p->set_trading_day(sys_days{2099y / December / 1}); // 3 and 17 days out
    using By = model::LegRule::ExpiryBy;
    const auto chain = [&](By by, int dte, int lo = 0, int hi = 0) {
        return model::leg_chain(*p, {.ratio = 1, .expiry_by = by, .dte = dte, .min_dte = lo,
                                     .max_dte = hi});
    };
    EXPECT_EQ(chain(By::kFirstAtLeast, 0), "SPY_20991204");
    EXPECT_EQ(chain(By::kFirstAtLeast, 5), "SPY_20991218");
    EXPECT_EQ(chain(By::kExactly, 3), "SPY_20991204");
    EXPECT_EQ(chain(By::kExactly, 4), "");
    EXPECT_EQ(chain(By::kNearest, 10, 0, 30), "SPY_20991204"); // 7 days either way: the earlier
    EXPECT_EQ(chain(By::kNearest, 15, 0, 30), "SPY_20991218");
    EXPECT_EQ(chain(By::kNearest, 15, 0, 10), "SPY_20991204"); // the window first
}

TEST(PortfolioLegs, ALegsRulePicksItsStrikeOffTheForwardByImpliedMovesOrByDelta) {
    // Quoted at their Black prices on a forward of 600 (vol 20%), a dime wide.
    const std::vector<double> strikes{570, 580, 590, 600, 610, 620, 630};
    std::vector<std::string> names;
    for (const double k : strikes) {
        names.push_back(symbol(OptionType::CALL, k));
        names.push_back(symbol(OptionType::PUT, k));
    }
    auto p = std::make_unique<model::Portfolio>("SPY");
    p->set_underlying(contract("SPY", 1.0));
    std::vector<model::Contract> listed;
    for (const std::string& n : names) {
        listed.push_back(contract(n, 100.0));
    }
    p->list(std::make_shared<const model::Listing>(listed));
    p->set_trading_day(sys_days{2099y / December / 1});
    const double years = model::years_to_expiry(kQuoted, model::DateTime{sys_days{kExpiry} + 21h});
    std::map<std::string, std::pair<double, double>> quotes;
    for (const double k : strikes) {
        for (const bool call : {true, false}) {
            const double v = quant::black_price(call, 600.0, k, years, 0.0, 0.2);
            quotes[symbol(call ? OptionType::CALL : OptionType::PUT, k)] = {v - 0.05, v + 0.05};
        }
    }
    model::PortfolioSnapshot s = snapshot(*p, 600.0, quotes);
    price(s);
    p->apply(s);
    const model::Chain chain = *p->chain(p->chains_by_expiry(0, 3650).front());
    using By = model::LegRule::StrikeBy;
    const auto strike = [&](OptionType right, By by, double value) {
        return model::leg_option(chain, {.ratio = 1, .right = right, .strike_by = by,
                                         .strike = value})
            ->strike();
    };
    EXPECT_DOUBLE_EQ(strike(OptionType::CALL, By::kForwardPct, 1.0), 610.0); // 606: nearest 610
    EXPECT_DOUBLE_EQ(strike(OptionType::PUT, By::kForwardPct, 0.0), 600.0);
    // The straddle's mid over the forward is the move; a move down from the forward.
    const double straddle = chain.call(600)->mid() + chain.put(600)->mid();
    const double down = 600.0 - straddle;
    const double expected = *std::ranges::min_element(
        strikes, {}, [down](double k) { return std::abs(k - down); });
    EXPECT_DOUBLE_EQ(strike(OptionType::PUT, By::kImpliedMoves, -1.0), expected);
    // By delta: the put nearest -0.25 of those priced.
    const double picked = strike(OptionType::PUT, By::kDelta, -0.25);
    for (const double k : strikes) {
        const model::Option put = *chain.put(k);
        EXPECT_LE(std::abs((chain.put(picked)->delta() / 100.0) + 0.25),
                  std::abs((put.delta() / 100.0) + 0.25) + 1e-12);
    }
}

} // namespace
