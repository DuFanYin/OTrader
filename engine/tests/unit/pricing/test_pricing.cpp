/**
 * Pricing (pricing::price_snapshot): implied vol from the side the mode names; Greeks priced when
 * asked from what a pricing keeps, its own bit for bit; each expiry at the rate to it on the curve;
 * a forward from parity only near the spot; priced again after a move, on the pool or here, what
 * pricing it all gives; an expiry's time moving in steps live. Smiles (pricing::fit_smiles,
 * quant::svi_fit): the smile the quotes came from given back, the same on any thread, butterfly
 * arbitrage found, fitted again once a minute live.
 */
#include "pricing/pricing.hpp"
#include "model/portfolio.hpp"
#include "model/portfolios.hpp"
#include "model/symbol.hpp"
#include "model/utility.hpp"
#include "quant/engine_api.h"
#include "utilities/thread_pool.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <gtest/gtest.h>
#include <initializer_list>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono;
using model::OptionType;

const year_month_day kExpiry = 2099y / December / 18;
// 10:00 New York, a few weeks before the expiry.
const model::DateTime kQuoted = sys_days{2099y / December / 1} + 15h;
// 10:00 New York on the expiry: six hours before its close.
const model::DateTime kExpiryMorning = sys_days{kExpiry} + 15h;

auto symbol(OptionType type, double strike, year_month_day expiry = kExpiry) -> std::string {
    return model::option_symbol({.root = "SPY", .expiry = expiry, .type = type, .strike = strike});
}

auto contract(const std::string& symbol, double size) -> model::Contract {
    model::Contract c;
    c.symbol = symbol;
    c.multiplier = size;
    return c;
}

/** SPY listing these options, and its underlying. */
auto portfolio(std::initializer_list<std::string> options) -> std::unique_ptr<model::Portfolio> {
    auto p = std::make_unique<model::Portfolio>("SPY");
    p->set_underlying(contract("SPY", 1.0));
    std::vector<model::Contract> listed;
    for (const std::string& o : options) {
        listed.push_back(contract(o, 100.0));
    }
    p->list(std::make_shared<const model::Listing>(std::move(listed)));
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

/** SPY's underlying, and a call and a put at every strike from low to high of each expiry. */
auto spy_contracts(std::initializer_list<year_month_day> expiries, int low, int high)
    -> std::vector<model::Contract> {
    std::vector<model::Contract> out{contract("SPY", 1.0)};
    for (const year_month_day expiry : expiries) {
        for (int k = low; k <= high; ++k) {
            for (const auto type : {OptionType::CALL, OptionType::PUT}) {
                out.push_back(contract(symbol(type, k, expiry), 100.0));
            }
        }
    }
    return out;
}

/** A snapshot of SPY at kExpiryMorning, spot 500, each option a little over its intrinsic value. */
auto quoted(const model::Portfolio& spy) -> model::PortfolioSnapshot {
    model::PortfolioSnapshot s = spy.snapshot();
    s.datetime = kExpiryMorning;
    s.underlying_bid = 499.9;
    s.underlying_ask = 500.1;
    const model::Listing& listing = spy.listing();
    for (std::size_t i = 0; i < listing.size(); ++i) {
        const double k = listing.strike(i);
        const bool call = listing.type(i) == OptionType::CALL;
        const double mid = std::max(call ? 500.0 - k : k - 500.0, 0.0) + 1.5;
        s.values(&model::Frame::bid)[i] = mid - 0.05;
        s.values(&model::Frame::ask)[i] = mid + 0.05;
    }
    return s;
}

/** All of it, here, at the default curve from the mid. */
void price(model::PortfolioSnapshot& s) { pricing::price_snapshot(s, model::RateCurve{}, "mid"); }

auto copy(std::span<const double> values) -> std::vector<double> {
    return {values.begin(), values.end()};
}

/** Values the same to rounding: what rescaling gives against pricing again. */
void expect_close(std::span<const double> got, std::span<const double> want, const char* what) {
    ASSERT_EQ(got.size(), want.size()) << what;
    for (std::size_t i = 0; i < got.size(); ++i) {
        EXPECT_NEAR(got[i], want[i], 1e-12 * std::max(1.0, std::abs(want[i])))
            << what << " at " << i;
    }
}

TEST(Pricing, ImpliedVolIsSolvedFromTheSideTheModeNames) {
    const std::string call = symbol(OptionType::CALL, 600);
    auto p = portfolio({call});
    model::PortfolioSnapshot s = snapshot(*p, 600.0, {{call, {10.0, 12.0}}});
    const auto iv = [&](std::string_view mode) {
        model::PortfolioSnapshot priced = s;
        pricing::price_snapshot(priced, model::RateCurve{}, mode);
        EXPECT_TRUE(priced.priced);
        return priced.iv[0];
    };
    const double mid = iv("mid");
    EXPECT_GT(mid, 0.0);
    const double bid = iv(model::iv_price_mode_of("BID"));
    const double ask = iv(model::iv_price_mode_of("Ask"));
    EXPECT_LT(bid, mid);
    EXPECT_LT(mid, ask);
    EXPECT_TRUE(model::iv_price_mode_of("last").empty()); // no such mode

    s.values(&model::Frame::bid)[0] = 0.0; // asked only: nothing to solve from
    model::PortfolioSnapshot unquoted = s;
    pricing::price_snapshot(unquoted, model::RateCurve{}, "bid");
    EXPECT_TRUE(unquoted.priced);
    EXPECT_EQ(unquoted.iv[0], 0.0);
    EXPECT_EQ(unquoted.delta[0], 0.0);

    model::PortfolioSnapshot unlisted;
    pricing::price_snapshot(unlisted, model::RateCurve{}, "mid");
    EXPECT_FALSE(unlisted.priced);
}

TEST(Pricing, GreeksPricedWhenAskedFromWhatAPricingKeepsAreItsOwnBitForBit) {
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
            pricing::price_snapshot(s, model::RateCurve::flat(0.05), mode);
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

TEST(Pricing, EachExpiryIsPricedAtTheRateToItAsAtThatOneRate) {
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
    pricing::price_snapshot(s, curve, "mid");
    ASSERT_EQ(s.expiry_rate.size(), 2U);
    for (std::size_t e = 0; e < 2; ++e) {
        EXPECT_EQ(s.expiry_rate[e], curve.at(s.expiry_years[e]));
    }
    EXPECT_NE(s.expiry_rate[0], s.expiry_rate[1]);
    const auto bits = [](double v) { return std::bit_cast<std::uint64_t>(v); };
    const model::Listing& listing = p->listing();
    for (std::size_t e = 0; e < 2; ++e) {
        model::PortfolioSnapshot one = snapshot(*p, 600.0, quotes);
        pricing::price_snapshot(one, model::RateCurve::flat(s.expiry_rate[e]), "mid");
        for (std::size_t i = 0; i < listing.size(); ++i) {
            if (listing.chains()[listing.chain_of(i)].expiry_index == e) {
                EXPECT_EQ(bits(s.iv[i]), bits(one.iv[i])) << e << " " << i;
                EXPECT_EQ(bits(s.delta[i]), bits(one.delta[i])) << e << " " << i;
            }
        }
    }
}

TEST(Pricing, AForwardFromParityNeedsAStrikeQuotedBothWaysNearTheSpot) {
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
        pricing::price_snapshot(s, model::RateCurve{}, "mid");
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

/** A chain big enough that pricing runs on the shared pool: priced again after the underlying
 * alone moved (each option's own flags read on the pool's threads), it is what pricing it all
 * gives (delta and gamma rescaled: to rounding). */
TEST(Pricing, BigChainRepricedOnPoolAfterUnderlyingMove) {
    model::Portfolios structure;
    structure.ensure_portfolio("SPY");
    structure.load_contracts(
        spy_contracts({2099y / December / 18, 2099y / December / 19}, 400, 599));
    const model::Portfolio* spy = structure.find_portfolio("SPY");
    const std::size_t n = spy->listing().size();
    ASSERT_GT(n, 256U); // above the pool's inline threshold

    model::PortfolioSnapshot s = quoted(*spy);
    price(s);
    ASSERT_TRUE(s.priced);

    s.underlying_bid = 501.9; // the underlying alone moves
    s.underlying_ask = 502.1;
    const std::vector<std::uint8_t> unchanged(n, 0);
    pricing::price_snapshot(s, model::RateCurve{}, "mid", unchanged, true, {},
                            &utilities::shared_pool());
    model::PortfolioSnapshot whole = s;
    whole.priced = false;
    price(whole); // here, not on the pool: the same values
    EXPECT_EQ(copy(s.iv), copy(whole.iv));
    expect_close(s.delta, whole.delta, "delta");
    EXPECT_EQ(copy(s.forward), copy(whole.forward));
}

/** The underlying alone moving rescales delta and gamma (the forward comes from the options' own
 * quotes): what pricing it all gives, to rounding. Live, an expiry's time moves in steps of
 * kLiveTimeStep while more than an hour is left, and every second under it. A same-day chain and a
 * three-day one. */
TEST(Pricing, AnUnderlyingTickRescalesAndTimeMovesInSteps) {
    model::Portfolios structure;
    structure.ensure_portfolio("SPY");
    structure.load_contracts(
        spy_contracts({2099y / December / 18, 2099y / December / 21}, 480, 520));
    const model::Portfolio* spy = structure.find_portfolio("SPY");

    model::PortfolioSnapshot s = quoted(*spy); // six hours before the same-day chain's close
    price(s);
    const std::vector<std::uint8_t> unchanged(spy->listing().size(), 0);
    const auto step = pricing::kLiveTimeStep;
    auto whole = [&] {
        model::PortfolioSnapshot w = s;
        w.priced = false;
        price(w);
        return w;
    };

    for (const double move : {0.25, 3.0, -7.5}) { // small and large moves alike: exact
        s.underlying_bid += move;
        s.underlying_ask += move;
        pricing::price_snapshot(s, model::RateCurve{}, "mid", unchanged, true, step);
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
    pricing::price_snapshot(s, model::RateCurve{}, "mid", unchanged, false, step);
    EXPECT_EQ(copy(s.expiry_years), years);
    s.datetime += 6s;
    pricing::price_snapshot(s, model::RateCurve{}, "mid", unchanged, false, step);
    EXPECT_NE(copy(s.expiry_years), years);
    EXPECT_EQ(copy(s.expiry_years), copy(whole().expiry_years));
    EXPECT_EQ(copy(s.iv), copy(whole().iv));

    // Under an hour before the same-day close, every second counts there.
    s.datetime = sys_days{2099y / December / 18} + 20h + 30min; // 15:30 New York
    pricing::price_snapshot(s, model::RateCurve{}, "mid", unchanged, false, step);
    const std::vector<double> late = copy(s.expiry_years);
    s.datetime += 1s;
    pricing::price_snapshot(s, model::RateCurve{}, "mid", unchanged, false, step);
    EXPECT_NE(s.expiry_years[0], late[0]);
    EXPECT_EQ(s.expiry_years[1], late[1]); // the three-day chain: within its step
}

// ---- Smiles

// 19.7% at the money, 17 days out: kQuoted to the expiry's close.
const quant::Svi kSmile{.a = 0.0012, .b = 0.012, .rho = -0.5, .m = 0.005, .sigma = 0.05};

/** SPY's underlying, and a call and a put every 5 from 440 to 560 of each expiry. */
auto smile_contracts(std::initializer_list<year_month_day> expiries)
    -> std::vector<model::Contract> {
    std::vector<model::Contract> out{contract("SPY", 1.0)};
    for (const year_month_day expiry : expiries) {
        for (int k = 440; k <= 560; k += 5) {
            for (const auto type : {OptionType::CALL, OptionType::PUT}) {
                out.push_back(contract(symbol(type, k, expiry), 100.0));
            }
        }
    }
    return out;
}

/** At kQuoted, spot 500, each option at Black on the spot grown at the default rate with kSmile's
 * vol at its strike (the same smile at every expiry), quoted a cent or 2% either side; one worth
 * under two cents not bid. */
auto smile_quoted(const model::Portfolio& spy) -> model::PortfolioSnapshot {
    model::PortfolioSnapshot s = spy.snapshot();
    s.datetime = kQuoted;
    s.underlying_bid = 499.99;
    s.underlying_ask = 500.01;
    const model::Listing& listing = spy.listing();
    const double r = model::RateCurve{}.at(0.0);
    for (std::size_t i = 0; i < listing.size(); ++i) {
        const model::DateTime expiry = listing.chains()[listing.chain_of(i)].expiry;
        const double t = model::years_to_expiry(kQuoted, expiry);
        const double f = 500.0 * std::exp(r * t);
        const double k = listing.strike(i);
        const double vol = std::sqrt(kSmile.total_variance(std::log(k / f)) / t);
        const double mid = quant::black_price(listing.type(i) == OptionType::CALL, f, k, t, r, vol);
        const double half = std::max(0.01, 0.02 * mid);
        s.values(&model::Frame::bid)[i] = mid < 0.02 ? 0.0 : mid - half;
        s.values(&model::Frame::ask)[i] = mid + half;
    }
    return s;
}

TEST(Smile, AFitGivesBackTheSmileItsQuotesCameFrom) {
    model::Portfolios structure;
    structure.ensure_portfolio("SPY");
    structure.load_contracts(smile_contracts({kExpiry}));
    model::Portfolio* spy = structure.find_portfolio("SPY");
    model::PortfolioSnapshot s = smile_quoted(*spy);
    price(s);
    pricing::fit_smiles(s);
    const double t = s.expiry_years[0];
    spy->apply(s); // s holds the portfolio's values before now

    const model::Chain chain = *spy->chain("SPY_20991218");
    const quant::SviFit fit = chain.smile();
    EXPECT_EQ(fit.points, 25); // one option a strike, the one out of the money
    EXPECT_TRUE(fit.butterfly_free);
    EXPECT_LT(fit.error, 1e-3); // in half spreads
    for (int k = 440; k <= 560; k += 5) {
        const double vol = std::sqrt(kSmile.total_variance(std::log(k / chain.forward())) / t);
        EXPECT_NEAR(*chain.smile_iv(k), vol, 1e-5) << k;
    }
    EXPECT_NEAR(*chain.atm_iv(), std::sqrt(kSmile.total_variance(0.0) / t), 1e-5);
    // The put's side steeper (rho < 0): its 25 delta IV over the call's.
    const double put = *chain.smile_iv_at_delta(-0.25);
    const double call = *chain.smile_iv_at_delta(0.25);
    EXPECT_GT(put, call);
    EXPECT_DOUBLE_EQ(*chain.skew_25d(), call - put);
    EXPECT_FALSE(chain.smile_iv_at_delta(1.0));
}

TEST(Smile, TheSamePointsGiveTheSameFitOnAnyThread) {
    model::Portfolios structure;
    structure.ensure_portfolio("SPY");
    structure.load_contracts(smile_contracts({kExpiry, 2099y / December / 21}));
    const model::Portfolio* spy = structure.find_portfolio("SPY");
    model::PortfolioSnapshot here = smile_quoted(*spy);
    price(here);
    model::PortfolioSnapshot pooled = here;
    pricing::fit_smiles(here);
    pricing::fit_smiles(pooled, {}, &utilities::shared_pool());
    EXPECT_GT(here.smile[model::kSmilePoints], 0.0);
    EXPECT_EQ(copy(here.smile), copy(pooled.smile));

    std::vector<double> k, w, half_spread;
    for (double x = -0.1; x <= 0.1; x += 0.01) {
        k.push_back(x);
        w.push_back(kSmile.total_variance(x) * (1.0 + (0.01 * std::sin(400.0 * x)))); // noisy
        half_spread.push_back(1e-5);
    }
    const quant::SviFit first = quant::svi_fit(k, w, half_spread);
    quant::SviFit other;
    std::jthread([&] { other = quant::svi_fit(k, w, half_spread); }).join();
    using Bits = std::array<double, 5>;
    EXPECT_EQ(std::bit_cast<Bits>(first.svi), std::bit_cast<Bits>(other.svi));
    EXPECT_EQ(first.error, other.error);
}

TEST(Smile, ArbitrageIsFoundAndTooFewQuotesAreNoFit) {
    // Axel Vogt's raw SVI: a density below zero near the money.
    const quant::Svi vogt{.a = -0.0410, .b = 0.1331, .rho = 0.3060, .m = 0.3586, .sigma = 0.4153};
    EXPECT_FALSE(vogt.butterfly_free(-1.5, 1.5));
    EXPECT_TRUE(kSmile.butterfly_free(-1.5, 1.5));
    // A later expiry's smile below this one's somewhere: calendar arbitrage there.
    quant::Svi later = kSmile;
    later.a += 0.001;
    EXPECT_TRUE(kSmile.calendar_free(later, -0.2, 0.2));
    later.rho = 0.5; // its put side below
    EXPECT_FALSE(kSmile.calendar_free(later, -0.2, 0.2));

    const std::vector<double> k{-0.02, -0.01, 0.0, 0.01};
    const std::vector<double> w{0.002, 0.0019, 0.0018, 0.0019};
    const std::vector<double> half_spread(4, 1e-5);
    EXPECT_EQ(quant::svi_fit(k, w, half_spread).points, 0);

    model::Portfolios structure;
    structure.ensure_portfolio("SPY");
    structure.load_contracts(smile_contracts({kExpiry}));
    model::Portfolio* spy = structure.find_portfolio("SPY");
    model::PortfolioSnapshot s = spy->snapshot(); // no quotes
    s.datetime = kQuoted;
    s.underlying_bid = 499.99;
    s.underlying_ask = 500.01;
    price(s);
    pricing::fit_smiles(s);
    EXPECT_GT(s.smile[model::kSmileYears], 0.0); // tried at this time
    spy->apply(s);
    EXPECT_EQ(spy->chain("SPY_20991218")->smile().points, 0);
    EXPECT_FALSE(spy->chain("SPY_20991218")->smile_iv(500.0));
}

/** Live, a chain's smile is fitted again once its last fit is kLiveFitStep old (by its time
 * left): kept through new quotes until then, then what fitting it now gives. */
TEST(Smile, LiveAChainIsFittedAgainOnceAMinute) {
    model::Portfolios structure;
    structure.ensure_portfolio("SPY");
    structure.load_contracts(smile_contracts({kExpiry}));
    const model::Portfolio* spy = structure.find_portfolio("SPY");
    model::PortfolioSnapshot s = smile_quoted(*spy);
    price(s);
    pricing::fit_smiles(s, pricing::kLiveFitStep);
    const std::vector<double> fitted = copy(s.smile);
    ASSERT_GT(s.smile[model::kSmilePoints], 0.0);

    std::vector<std::uint8_t> changed(spy->listing().size(), 1);
    const auto requote = [&](std::chrono::seconds later) {
        s.datetime += later;
        for (const auto side : {&model::Frame::bid, &model::Frame::ask}) {
            for (double& v : s.values(side)) {
                v *= 1.01;
            }
        }
        pricing::price_snapshot(s, model::RateCurve{}, "mid", changed, false,
                                pricing::kLiveTimeStep);
        pricing::fit_smiles(s, pricing::kLiveFitStep);
    };
    requote(30s);
    EXPECT_EQ(copy(s.smile), fitted);
    requote(31s);
    EXPECT_NE(copy(s.smile), fitted);
    model::PortfolioSnapshot now = s;
    std::ranges::fill(now.values(&model::Frame::smile), 0.0);
    pricing::fit_smiles(now);
    EXPECT_EQ(copy(s.smile), copy(now.smile));
}

} // namespace
