/** Pricing: parity forwards, then IVs and Greeks, all of a snapshot or what moved; then smiles. */

#include "pricing/pricing.hpp"
#include "model/portfolio.hpp"
#include "model/symbol.hpp"
#include "model/utility.hpp"
#include "quant/engine_api.h"
#include "utilities/fnv.hpp"
#include "utilities/thread_pool.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace pricing {

using model::Chain;
using model::DateTime;
using model::Frame;
using model::IvInput;
using model::Listing;
using model::OptionType;
using model::PortfolioSnapshot;
using model::RateCurve;
using model::cannot_price;
using model::frame_spot;
using model::iv_input;
using model::iv_input_price;
using model::mark;
using model::pricing_forward;
using model::years_to_expiry;

namespace {

/** Parity's median over the three strikes nearest the money quoted both ways; 0 for none (its
 * options are priced on the spot grown, pricing_forward). */
auto chain_forward(const Listing::Chain& chain, const Frame& frame, double spot, double rate)
    -> double {
    if (spot <= 0.0) {
        return 0.0;
    }
    const double t = frame.expiry_years[chain.expiry_index];
    if (t <= 0.0) {
        return 0.0;
    }
    // C - P = e^(-rt) (F - K)
    struct Estimate {
        double gap; // |C - P|: smallest at the strikes nearest the forward
        double forward;
    };
    thread_local std::vector<Estimate> estimates;
    thread_local std::vector<double> forwards;
    estimates.clear();
    const double grow = std::exp(rate * t);
    for (std::size_t j = 0; j < chain.strikes.size(); ++j) {
        if (chain.calls[j] < 0 || chain.puts[j] < 0) {
            continue;
        }
        const auto c = static_cast<std::size_t>(chain.calls[j]);
        const auto p = static_cast<std::size_t>(chain.puts[j]);
        if (!(frame.bid[c] > 0.0 && frame.ask[c] >= frame.bid[c] && frame.bid[p] > 0.0 &&
              frame.ask[p] >= frame.bid[p])) {
            continue;
        }
        const double gap = mark(frame.bid[c], frame.ask[c]) - mark(frame.bid[p], frame.ask[p]);
        estimates.push_back({std::abs(gap), chain.strikes[j] + (grow * gap)});
    }
    std::optional<double> implied;
    if (!estimates.empty()) {
        // A median: one stale quote does not move it.
        const auto nearest = std::min<std::ptrdiff_t>(3, std::ssize(estimates));
        std::ranges::partial_sort(estimates, estimates.begin() + nearest, {}, &Estimate::gap);
        forwards.clear();
        for (const Estimate& e : estimates | std::views::take(nearest)) {
            forwards.push_back(e.forward);
        }
        std::ranges::sort(forwards);
        const std::size_t mid = forwards.size() / 2;
        implied = forwards.size() % 2 == 1 ? forwards[mid]
                                           : 0.5 * (forwards[mid - 1] + forwards[mid]);
    }
    return implied && std::abs((*implied / spot) - 1.0) < Chain::kMaxForwardGap ? *implied : 0.0;
}

/** Of the mid, the widest spread a smile is fitted to: wider, the price is mostly the tick. */
constexpr double kSmileMaxSpread = 0.5;

/** Chain c's smile fitted, at its forward and time now (fit_smiles). */
void fit_smile(PortfolioSnapshot& snapshot, std::size_t c) {
    const Listing::Chain& chain = snapshot.listing->chains()[c];
    const std::span<double> out =
        snapshot.values(&Frame::smile).subspan(c * model::kSmileValues, model::kSmileValues);
    const double spot = frame_spot(snapshot);
    const double t = snapshot.expiry_years[chain.expiry_index];
    const double r = snapshot.expiry_rate[chain.expiry_index];
    std::ranges::fill(out, 0.0);
    out[model::kSmileYears] = t;
    if (spot <= 0.0 || t <= 0.0) {
        return;
    }
    const double f = pricing_forward(snapshot.forward[c], spot, r, t);
    out[model::kSmileForward] = f;
    thread_local std::vector<double> k;
    thread_local std::vector<double> w;
    thread_local std::vector<double> half_spread;
    k.clear();
    w.clear();
    half_spread.clear();
    const double discount = std::exp(-r * t);
    const double root_t = std::sqrt(t);
    constexpr double kInvRootTwoPi = 0.3989422804014327;
    for (std::size_t j = 0; j < chain.strikes.size(); ++j) {
        const double strike = chain.strikes[j];
        const std::int32_t column = strike < f ? chain.puts[j] : chain.calls[j];
        if (column < 0) {
            continue;
        }
        const auto i = static_cast<std::size_t>(column);
        const double bid = snapshot.bid[i];
        const double ask = snapshot.ask[i];
        const double vol = snapshot.iv[i];
        if (!(bid > 0.0 && ask >= bid && vol > 0.0 &&
              ask - bid <= kSmileMaxSpread * 0.5 * (bid + ask))) {
            continue;
        }
        const double x = std::log(strike / f);
        const double sd = vol * root_t;
        const double d1 = (-x / sd) + (0.5 * sd);
        const double vega = discount * f * kInvRootTwoPi * std::exp(-0.5 * d1 * d1) * root_t;
        if (!(vega > 0.0)) {
            continue;
        }
        // Half the spread in vol, then in total variance (dw = 2 vol t dvol).
        k.push_back(x);
        w.push_back(sd * sd);
        half_spread.push_back(2.0 * vol * t * (0.5 * (ask - bid) / vega));
    }
    const quant::SviFit fit = quant::svi_fit(k, w, half_spread);
    if (fit.points == 0) {
        return;
    }
    out[model::kSviA] = fit.svi.a;
    out[model::kSviB] = fit.svi.b;
    out[model::kSviRho] = fit.svi.rho;
    out[model::kSviM] = fit.svi.m;
    out[model::kSviSigma] = fit.svi.sigma;
    out[model::kSmileError] = fit.error;
    out[model::kSmilePoints] = fit.points;
    out[model::kSmileButterflyFree] = fit.butterfly_free ? 1.0 : 0.0;
}

} // namespace

void price_snapshot(PortfolioSnapshot& snapshot, const RateCurve& rates,
                    std::string_view iv_price_mode, std::span<const std::uint8_t> quotes_changed,
                    bool underlying_changed, std::chrono::seconds time_step,
                    utilities::ThreadPool* pool) {
    if (snapshot.listing == nullptr) {
        snapshot.priced = false;
        return;
    }
    const Listing& listing = *snapshot.listing;
    const std::size_t n = listing.size();
    const std::span<const Listing::Chain> chains = listing.chains();
    const std::span<const DateTime> expiries = listing.expiries();
    const double spot = frame_spot(snapshot);
    const bool again_only =
        !quotes_changed.empty() && quotes_changed.size() == n && snapshot.priced && spot > 0.0;
    const double step_years =
        again_only ? static_cast<double>(time_step.count()) / model::kSecondsAYear : 0.0;
    const double min_left_years =
        static_cast<double>(std::chrono::seconds(kTimeStepMinTimeLeft).count()) /
        model::kSecondsAYear;
    const bool rescale = again_only && underlying_changed && snapshot.greeks_spot > 0.0;
    const double spot_ratio = rescale ? snapshot.greeks_spot / spot : 1.0;
    const std::span<double> iv = snapshot.values(&Frame::iv);
    const std::span<double> delta = snapshot.values(&Frame::delta);
    const std::span<double> gamma = snapshot.values(&Frame::gamma);
    const std::span<double> theta = snapshot.values(&Frame::theta);
    const std::span<double> vega = snapshot.values(&Frame::vega);
    const std::span<double> forward = snapshot.values(&Frame::forward);
    const std::span<double> expiry_years = snapshot.values(&Frame::expiry_years);
    const std::span<double> expiry_rate = snapshot.values(&Frame::expiry_rate);
    if (!again_only) {
        for (const std::span<double> values : {iv, delta, gamma, theta, vega, forward, expiry_years,
                                               expiry_rate, snapshot.values(&Frame::smile)}) {
            std::ranges::fill(values, 0.0);
        }
    }

    thread_local std::vector<std::uint8_t> expiry_moved;
    thread_local std::vector<std::uint8_t> chain_moved;
    expiry_moved.assign(expiries.size(), 1);
    chain_moved.assign(chains.size(), 1);
    bool any_expiry_moved = false;
    for (std::size_t e = 0; e < expiries.size(); ++e) {
        const double t = years_to_expiry(snapshot.datetime, expiries[e]);
        const bool within_step = std::abs(t - expiry_years[e]) < step_years &&
                                 t > min_left_years && expiry_years[e] > min_left_years;
        const bool moved = !again_only || (t != expiry_years[e] && !within_step);
        expiry_moved[e] = static_cast<std::uint8_t>(moved);
        any_expiry_moved = any_expiry_moved || moved;
        if (moved) {
            expiry_years[e] = t;
            expiry_rate[e] = rates.at(t);
        }
    }
    for (std::size_t c = 0; c < chains.size(); ++c) {
        const Listing::Chain& chain = chains[c];
        // Its quotes and time as before, and its parity forward still near the spot: the same.
        if (again_only && expiry_moved[chain.expiry_index] == 0 && forward[c] > 0.0 &&
            std::abs((forward[c] / spot) - 1.0) < Chain::kMaxForwardGap &&
            std::ranges::none_of(quotes_changed.subspan(chain.first, chain.count),
                                 [](std::uint8_t q) { return q != 0; })) {
            chain_moved[c] = 0;
            continue;
        }
        const double f = chain_forward(chain, snapshot, spot, expiry_rate[chain.expiry_index]);
        chain_moved[c] = static_cast<std::uint8_t>(!again_only || f != forward[c]);
        forward[c] = f;
    }

    const IvInput input = iv_input(iv_price_mode);
    // Spans: the pool's threads would see their own (empty) thread_locals.
    const std::span<const std::uint8_t> expiry_flags = expiry_moved;
    const std::span<const std::uint8_t> chain_flags = chain_moved;
    auto solve = [&](std::size_t i, bool again) {
        const std::uint32_t c = listing.chain_of(i);
        const std::uint32_t e = chains[c].expiry_index;
        const double k = listing.strike(i);
        const double t = expiry_years[e];
        const double r = expiry_rate[e];
        const bool is_call = listing.type(i) == OptionType::CALL;
        const double px = iv_input_price(snapshot.bid[i], snapshot.ask[i], input);
        if (cannot_price(spot, k, t, px)) {
            iv[i] = delta[i] = gamma[i] = theta[i] = vega[i] = 0.0;
            return;
        }
        const double f = pricing_forward(forward[c], spot, r, t);
        const double vol = again ? quant::black_implied_vol(px, f, k, t, r, is_call) : iv[i];
        const quant::BlackGreeks g = quant::black_greeks(is_call, spot, f, k, t, r, vol);
        iv[i] = vol;
        delta[i] = g.delta;
        gamma[i] = g.gamma;
        theta[i] = g.theta;
        vega[i] = g.vega;
    };
    // An expiry moved: the whole portfolio to the pool, as a whole pricing goes (what moved alone
    // can be under the pool's threshold and run here, one solve after another).
    if (!again_only || (underlying_changed && !rescale) || any_expiry_moved) {
        const auto price = [&](std::size_t i) {
            const std::uint32_t c = listing.chain_of(i);
            const bool again = !again_only || quotes_changed[i] != 0 ||
                               expiry_flags[chains[c].expiry_index] != 0 || chain_flags[c] != 0 ||
                               (forward[c] <= 0.0 && underlying_changed);
            if (!again && !underlying_changed) {
                return; // nothing it is priced from moved
            }
            if (!again && rescale) { // per unit of spot: delta once, gamma twice
                delta[i] *= spot_ratio;
                gamma[i] *= spot_ratio * spot_ratio;
                return;
            }
            solve(i, again);
        };
        if (pool != nullptr) {
            pool->parallel_for(0, n, price);
        } else {
            for (std::size_t i = 0; i < n; ++i) {
                price(i);
            }
        }
    } else {
        if (rescale) { // every option's, then what moved solved again
            const double gamma_ratio = spot_ratio * spot_ratio;
            for (std::size_t i = 0; i < n; ++i) {
                delta[i] *= spot_ratio;
                gamma[i] *= gamma_ratio;
            }
        }
        thread_local std::vector<std::uint32_t> moved;
        moved.clear();
        for (std::size_t c = 0; c < chains.size(); ++c) {
            const Listing::Chain& chain = chains[c];
            const bool whole = chain_flags[c] != 0 || (forward[c] <= 0.0 && underlying_changed);
            for (std::uint32_t i = chain.first; i < chain.first + chain.count; ++i) {
                if (whole || quotes_changed[i] != 0) {
                    moved.push_back(i);
                }
            }
        }
        for (const std::uint32_t i : moved) {
            solve(i, true);
        }
    }
    snapshot.priced = true;
    snapshot.greeks_spot = spot;
    snapshot.iv_price_mode = {}; // it holds its Greeks now
}

void fit_smiles(PortfolioSnapshot& snapshot, std::chrono::seconds step,
                utilities::ThreadPool* pool) {
    if (snapshot.listing == nullptr || !snapshot.priced) {
        return;
    }
    const std::span<const Listing::Chain> chains = snapshot.listing->chains();
    const double step_years = static_cast<double>(step.count()) / model::kSecondsAYear;
    thread_local std::vector<std::uint32_t> due;
    due.clear();
    for (std::size_t c = 0; c < chains.size(); ++c) {
        const double tried = snapshot.smile[(c * model::kSmileValues) + model::kSmileYears];
        const double t = snapshot.expiry_years[chains[c].expiry_index];
        if (step_years <= 0.0 || tried == 0.0 || tried - t >= step_years) {
            due.push_back(static_cast<std::uint32_t>(c));
        }
    }
    // Spans: the pool's threads would see their own (empty) thread_locals.
    const std::span<const std::uint32_t> chains_due = due;
    if (pool != nullptr && chains_due.size() > 1) {
        pool->parallel_for(
            0, chains_due.size(), [&](std::size_t i) { fit_smile(snapshot, chains_due[i]); },
            /*inline_threshold=*/1);
    } else {
        for (const std::uint32_t c : chains_due) {
            fit_smile(snapshot, c);
        }
    }
}

auto fingerprint() -> std::uint64_t {
    static const std::uint64_t value = [] {
        // SPY on 2025-03-03: three expiries, strikes 560 to 600, quotes with holes (none, one
        // side, crossed), at three times, the last with no underlying quote (its mark kept); each
        // IV mode.
        using namespace std::chrono;
        std::vector<model::Contract> contracts;
        for (const year_month_day expiry : {2025y / March / 3d, 2025y / March / 7d,
                                            2025y / March / 21d}) {
            for (double k = 560.0; k <= 600.0; k += 2.5) {
                for (const model::OptionType type : {model::OptionType::CALL,
                                                     model::OptionType::PUT}) {
                    model::Contract c;
                    c.symbol = model::option_symbol(
                        {.root = "SPY", .expiry = expiry, .type = type, .strike = k});
                    c.multiplier = 100.0;
                    contracts.push_back(std::move(c));
                }
            }
        }
        const auto listing = std::make_shared<const model::Listing>(std::move(contracts));
        const sys_days day{2025y / March / 3d};
        const std::array<model::DateTime, 3> times{day + 15h, day + 20h + 30min, day + 20h + 59min};
        utilities::Fnv hash;
        for (const std::string_view mode : {"mid", "bid", "ask"}) {
            for (std::size_t t = 0; t < times.size(); ++t) {
                model::PortfolioSnapshot s(listing, "SPY");
                s.datetime = times[t];
                if (t + 1 < times.size()) {
                    s.underlying_bid = 580.0 + (0.37 * static_cast<double>(t));
                    s.underlying_ask = s.underlying_bid + 0.02;
                }
                s.underlying_mark = 580.5;
                const std::span<double> bid = s.values(&model::Frame::bid);
                const std::span<double> ask = s.values(&model::Frame::ask);
                for (std::size_t i = 0; i < listing->size(); ++i) {
                    if ((i + t) % 7 == 3) {
                        continue; // not quoted
                    }
                    const double k = listing->strike(i);
                    const bool call = listing->type(i) == model::OptionType::CALL;
                    const double value = std::max(call ? 580.0 - k : k - 580.0, 0.0) +
                                         (2.0 + listing->chain_of(i)) *
                                             std::exp(-std::abs(k - 580.0) / 15.0);
                    bid[i] = i % 11 == 5 ? 0.0 : std::max(value - 0.05, 0.01);
                    ask[i] = value + 0.05;
                    if (i % 13 == 6) {
                        std::swap(bid[i], ask[i]); // crossed
                    }
                }
                price_snapshot(s, model::RateCurve::flat(0.05), mode);
                fit_smiles(s);
                for (const auto column : model::PortfolioSnapshot::kOptionColumns) {
                    hash.add(s.*column);
                }
                hash.add(s.forward);
                hash.add(s.expiry_years);
                hash.add(s.smile);
                hash.add(s.priced);
                hash.add(s.greeks_spot);
            }
        }
        return hash.value();
    }();
    return value;
}

} // namespace pricing
