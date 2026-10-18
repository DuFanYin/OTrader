/**
 * A portfolio's listing, frames and pricing (Portfolio), and the structure that holds the
 * portfolios (Portfolios): the columns' order; a snapshot's views after a copy or a move;
 * implied vol from the side the mode names; each expiry at the rate to it on the curve; Greeks
 * priced when asked from what a pricing keeps, its own bit for bit; a frame of another listing moves only the clock and the underlying; a
 * frame read in place or copied; a roll keeps what stays; the at-the-money strike and a chain's
 * forward; chains by expiry and a day's options in place of the ones before; contracts of no
 * portfolio.
 */
#include "model/portfolio.hpp"
#include "model/portfolios.hpp"
#include "model/portfolio_spec.hpp"
#include "model/symbol.hpp"
#include "model/utility.hpp"
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

TEST(PortfolioPricing, ImpliedVolIsSolvedFromTheSideTheModeNamesInAnyCase) {
    const std::string call = symbol(OptionType::CALL, 600);
    auto p = portfolio({call});
    model::PortfolioSnapshot s = snapshot(*p, 600.0, {{call, {10.0, 12.0}}});
    const auto iv = [&] {
        model::PortfolioSnapshot priced = s;
        p->price_snapshot(priced);
        EXPECT_TRUE(priced.priced);
        return priced.iv[0];
    };
    const double mid = iv();
    EXPECT_GT(mid, 0.0);
    p->set_iv_price_mode("BID");
    const double bid = iv();
    p->set_iv_price_mode("Ask");
    const double ask = iv();
    EXPECT_LT(bid, mid);
    EXPECT_LT(mid, ask);
    p->set_iv_price_mode("last"); // no such mode: the one before stays
    EXPECT_EQ(iv(), ask);
    p->set_risk_free_rate(std::nan("")); // ignored
    EXPECT_EQ(iv(), ask);

    p->set_iv_price_mode("bid");
    s.values(&model::Frame::bid)[0] = 0.0; // asked only: nothing to solve from
    model::PortfolioSnapshot unquoted = s;
    p->price_snapshot(unquoted);
    EXPECT_TRUE(unquoted.priced);
    EXPECT_EQ(unquoted.iv[0], 0.0);
    EXPECT_EQ(unquoted.delta[0], 0.0);

    model::PortfolioSnapshot unlisted;
    p->price_snapshot(unlisted);
    EXPECT_FALSE(unlisted.priced);
}

TEST(PortfolioPricing, GreeksPricedWhenAskedFromWhatAPricingKeepsAreItsOwnBitForBit) {
    // Two expiries: the later one quoted both ways (a forward by parity); the earlier only in calls
    // (the spot grown); quotes missing, one-sided, at zero; and a spot of none.
    const year_month_day near = 2099y / December / 4;
    std::vector<std::string> options;
    std::map<std::string, std::pair<double, double>> quotes;
    for (const double k : {580.0, 590.0, 600.0, 610.0, 620.0}) {
        for (const OptionType type : {OptionType::CALL, OptionType::PUT}) {
            const std::string later = symbol(type, k);
            options.push_back(later);
            const double value = std::max(type == OptionType::CALL ? 600.0 - k : k - 600.0, 0.0) +
                                 8.0 * std::exp(-std::abs(k - 600.0) / 20.0);
            if (k != 610.0 || type != OptionType::PUT) {
                quotes[later] = {k == 590.0 ? 0.0 : value - 0.07, value + 0.07};
            }
        }
        const std::string sooner = symbol(OptionType::CALL, k, near);
        options.push_back(sooner);
        quotes[sooner] = {std::max(600.0 - k, 0.0) + 1.0, std::max(600.0 - k, 0.0) + 1.3};
    }
    auto p = std::make_unique<model::Portfolio>("SPY");
    p->set_underlying(contract("SPY", 1.0));
    std::vector<model::Contract> listed;
    for (const std::string& o : options) {
        listed.push_back(contract(o, 100.0));
    }
    p->list(std::make_shared<const model::Listing>(listed));
    const auto bits = [](double v) { return std::bit_cast<std::uint64_t>(v); };
    for (const double spot : {600.3, 0.0}) {
        for (const std::string_view mode : {"mid", "bid", "ask"}) {
            model::PortfolioSnapshot s = snapshot(*p, spot, quotes);
            model::price_snapshot(s, model::RateCurve::flat(0.05), mode);
            model::Frame kept = s; // what a stored day keeps: no Greeks, the setting
            kept.delta = kept.gamma = kept.theta = kept.vega = {};
            kept.iv_price_mode = mode;
            bool some = false;
            for (std::size_t i = 0; i < s.iv.size(); ++i) {
                const model::Greeks g = model::greeks_at(kept, i);
                EXPECT_EQ(bits(g.delta), bits(s.delta[i])) << mode << " " << spot << " " << i;
                EXPECT_EQ(bits(g.gamma), bits(s.gamma[i])) << mode << " " << spot << " " << i;
                EXPECT_EQ(bits(g.theta), bits(s.theta[i])) << mode << " " << spot << " " << i;
                EXPECT_EQ(bits(g.vega), bits(s.vega[i])) << mode << " " << spot << " " << i;
                some = some || g.delta != 0.0;
            }
            EXPECT_EQ(some, spot > 0.0);
            // A snapshot that takes the frame holds them as its own.
            model::PortfolioSnapshot copy = p->snapshot();
            copy.assign(kept);
            for (std::size_t i = 0; i < s.iv.size(); ++i) {
                EXPECT_EQ(bits(copy.delta[i]), bits(s.delta[i])) << mode << " " << spot << " " << i;
            }
        }
    }
}

TEST(PortfolioPricing, ARateCurveIsStraightBetweenItsPointsAndFlatBeyondThem) {
    const model::RateCurve c{.years = {1.0 / 12, 1.0}, .rates = {0.01, 0.04}};
    EXPECT_EQ(c.at(0.0), 0.01);
    EXPECT_EQ(c.at(1.0 / 12), 0.01);
    EXPECT_DOUBLE_EQ(c.at(0.5), 0.01 + ((0.5 - (1.0 / 12)) / (1.0 - (1.0 / 12)) * 0.03));
    EXPECT_EQ(c.at(2.0), 0.04);
    EXPECT_EQ(model::RateCurve::flat(0.05).at(3.0), 0.05);
}

TEST(PortfolioPricing, EachExpiryIsPricedAtTheRateToItAsAtThatOneRate) {
    // Two expiries, 3 and 17 days out, whose rates on the curve differ.
    const year_month_day near = 2099y / December / 4;
    std::vector<std::string> options;
    std::map<std::string, std::pair<double, double>> quotes;
    for (const year_month_day expiry : {near, kExpiry}) {
        for (const double k : {590.0, 600.0, 610.0}) {
            for (const OptionType type : {OptionType::CALL, OptionType::PUT}) {
                const std::string o = symbol(type, k, expiry);
                options.push_back(o);
                const double value =
                    std::max(type == OptionType::CALL ? 600.0 - k : k - 600.0, 0.0) + 5.0;
                quotes[o] = {value - 0.05, value + 0.05};
            }
        }
    }
    auto p = std::make_unique<model::Portfolio>("SPY");
    p->set_underlying(contract("SPY", 1.0));
    std::vector<model::Contract> listed;
    for (const std::string& o : options) {
        listed.push_back(contract(o, 100.0));
    }
    p->list(std::make_shared<const model::Listing>(listed));
    const model::RateCurve curve{.years = {3.0 / 365, 30.0 / 365}, .rates = {0.0, 0.10}};
    model::PortfolioSnapshot s = snapshot(*p, 600.0, quotes);
    model::price_snapshot(s, curve, "mid");
    ASSERT_EQ(s.expiry_rate.size(), 2U);
    for (std::size_t e = 0; e < 2; ++e) {
        EXPECT_EQ(s.expiry_rate[e], curve.at(s.expiry_years[e]));
    }
    EXPECT_NE(s.expiry_rate[0], s.expiry_rate[1]);
    const auto bits = [](double v) { return std::bit_cast<std::uint64_t>(v); };
    const model::Listing& listing = p->listing();
    for (std::size_t e = 0; e < 2; ++e) {
        model::PortfolioSnapshot one = snapshot(*p, 600.0, quotes);
        model::price_snapshot(one, model::RateCurve::flat(s.expiry_rate[e]), "mid");
        for (std::size_t i = 0; i < listing.size(); ++i) {
            if (listing.chains()[listing.chain_of(i)].expiry_index == e) {
                EXPECT_EQ(bits(s.iv[i]), bits(one.iv[i])) << e << " " << i;
                EXPECT_EQ(bits(s.delta[i]), bits(one.delta[i])) << e << " " << i;
            }
        }
    }
}

TEST(PortfolioFrames, AFrameOfAnotherListingMovesOnlyTheClockAndTheUnderlying) {
    const std::string call = symbol(OptionType::CALL, 600);
    auto p = portfolio({call});
    model::PortfolioSnapshot s = snapshot(*p, 600.0, {{call, {1.0, 1.2}}});
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
    p->apply(s); // priced here, then taken over: s has the zeros it had
    EXPECT_TRUE(p->frame().priced);
    EXPECT_DOUBLE_EQ(s.bid[0], 0.0);
    const model::Option option = *p->option(call);
    EXPECT_DOUBLE_EQ(option.bid(), 10.0);
    EXPECT_DOUBLE_EQ(option.mid(), 11.0);
    EXPECT_DOUBLE_EQ(option.bid_size(), 5.0);
    EXPECT_GT(option.iv(), 0.0);
    EXPECT_DOUBLE_EQ(option.delta(), p->frame().delta[0] * 100.0);

    auto stored = std::make_shared<model::PortfolioSnapshot>(snapshot(*p, 600.0, {{call, {7, 8}}}));
    p->price_snapshot(*stored);
    p->apply(*stored, stored);
    EXPECT_EQ(p->frame().bid.data(), stored->bid.data()); // read where it is
    stored->values(&model::Frame::bid)[0] = 7.5;
    EXPECT_DOUBLE_EQ(p->option(call)->bid(), 7.5);

    model::PortfolioSnapshot copied = snapshot(*p, 600.0, {{call, {3.0, 4.0}}});
    p->price_snapshot(copied);
    p->apply(static_cast<const model::Frame&>(copied));
    copied.values(&model::Frame::bid)[0] = 0.0;
    EXPECT_DOUBLE_EQ(p->option(call)->bid(), 3.0); // a copy of its own
}

TEST(PortfolioFrames, AHedgeIsCountedOnlyWhenBothArePriced) {
    const std::string call = symbol(OptionType::CALL, 600);
    auto p = portfolio({call});
    p->set_hedge_instrument(contract("SPYX", 1.0));
    model::PortfolioSnapshot s = snapshot(*p, 600.0, {{call, {10.0, 12.0}}});
    p->apply(s);
    EXPECT_DOUBLE_EQ(p->hedge()->theo_delta(), 0.0); // not priced
    EXPECT_DOUBLE_EQ(p->underlying()->theo_delta(), 1.0);

    s = snapshot(*p, 600.0, {{call, {10.0, 12.0}}});
    s.hedge_bid = 299.9;
    s.hedge_ask = 300.1;
    s.hedge_mark = 300.0;
    p->apply(s);
    EXPECT_DOUBLE_EQ(p->hedge()->theo_delta(), 0.5);
    EXPECT_EQ(p->instrument("SPYX")->symbol(), "SPYX");
    EXPECT_FALSE(p->instrument("DIA"));

    model::PortfolioSnapshot no_spot = snapshot(*p, 0.0, {{call, {10.0, 12.0}}});
    no_spot.hedge_mark = 300.0;
    p->apply(no_spot);
    EXPECT_DOUBLE_EQ(p->hedge()->theo_delta(), 0.0);
}

TEST(PortfolioFrames, ARollKeepsTheValuesOfWhatStays) {
    const std::string stays = symbol(OptionType::CALL, 600);
    const std::string goes = symbol(OptionType::CALL, 610);
    const std::string comes = symbol(OptionType::CALL, 600, 2099y / December / 19);
    auto p = portfolio({stays, goes});
    model::PortfolioSnapshot s = snapshot(*p, 600.0, {{stays, {10.0, 12.0}}, {goes, {5, 6}}});
    p->apply(s);
    const double forward = p->chain("SPY_20991218")->forward();
    EXPECT_GT(forward, 0.0);

    EXPECT_TRUE(p->list(std::make_shared<const model::Listing>(contracts({stays, comes}))).empty());
    EXPECT_DOUBLE_EQ(p->option(stays)->bid(), 10.0);
    EXPECT_DOUBLE_EQ(p->option(comes)->bid(), 0.0);
    EXPECT_FALSE(p->option(goes));
    EXPECT_DOUBLE_EQ(p->chain("SPY_20991218")->forward(), forward);
    EXPECT_DOUBLE_EQ(p->underlying()->mid(), 600.0);
    EXPECT_EQ(p->datetime(), kQuoted);
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

TEST(PortfolioPricing, AForwardFromParityNeedsAStrikeQuotedBothWaysNearTheSpot) {
    const std::string c600 = symbol(OptionType::CALL, 600);
    const std::string p600 = symbol(OptionType::PUT, 600);
    const std::string c610 = symbol(OptionType::CALL, 610);
    const std::string p610 = symbol(OptionType::PUT, 610);
    auto p = portfolio({c600, p600, c610, p610});
    const auto priced = [&](double spot, double call_600) {
        model::PortfolioSnapshot s = snapshot(
            *p, spot,
            {{c600, {call_600 - 0.1, call_600 + 0.1}},
             {p600, {4.9, 5.1}},
             {c610, {1.0, 1.2}},
             {p610, {0.0, 10.2}}}); // asked only: no parity there
        p->price_snapshot(s);
        return s;
    };
    const model::PortfolioSnapshot parity = priced(600.0, 5.5);
    const double grow = std::exp(0.05 * parity.expiry_years[0]);
    EXPECT_NEAR(parity.forward[0], 600.0 + (grow * 0.5), 1e-9);

    model::PortfolioSnapshot far = priced(600.0, 200.0); // a forward near 795: bad quotes
    EXPECT_EQ(far.forward[0], 0.0);                      // none by parity
    p->apply(far);
    EXPECT_NEAR(p->chain("SPY_20991218")->forward(), 600.0 * grow, 1e-9); // the spot grown

    EXPECT_EQ(priced(0.0, 5.5).forward[0], 0.0); // no spot
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

    structure.ensure_portfolio("ONLY_HEDGED");
    structure.find_portfolio("ONLY_HEDGED")->hedge_symbol = "DIA";
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

} // namespace
