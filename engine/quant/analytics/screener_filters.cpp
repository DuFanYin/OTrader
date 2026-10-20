#include "screener.h"
#include "quant/kernel.h"

#include <cmath>
#include <omp.h>

namespace quant {

namespace {
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
}

// ── Black-76 ──────────────────────────────────────────────────────────────────

double black76_price(double forward, double strike, double rate, double vol, double years, bool is_call) {
    if (forward <= 0.0 || strike <= 0.0) return 0.0;
    if (years <= 0.0) return std::max(is_call ? forward - strike : strike - forward, 0.0);
    return black_price(is_call, forward, strike, years, rate, vol);
}

void black76_greeks(double forward, double strike, double rate, double vol, double years, bool is_call,
                    double* out_delta, double* out_gamma, double* out_theta_day, double* out_vega_pt) {
    // to the forward: black_greeks with the spot at the forward
    const BlackGreeks g = black_greeks(is_call, forward, forward, strike, years, rate, vol);
    *out_delta     = g.delta;
    *out_gamma     = g.gamma;
    *out_vega_pt   = g.vega;
    *out_theta_day = g.theta;
}

// ── Option-level primitives ───────────────────────────────────────────────────

double screen_mid(const ChainOption& o) {
    if (std::isfinite(o.bid) && std::isfinite(o.ask)) return 0.5 * (o.bid + o.ask);
    return o.mark;
}

double screen_fill_price(const ChainOption& o, int qty, ScreenPriceMode mode) {
    if (mode == ScreenPriceMode::Mid) {
        const double mid = screen_mid(o);
        return mid > 0.0 ? mid : 0.0;
    }
    const double px = qty > 0 ? o.ask : o.bid;
    return (std::isfinite(px) && px > 0.0) ? px : kNaN;
}

std::vector<int> screen_filter_options(std::span<const ChainOption> chain, const ScreenOptionFilter& f) {
    std::vector<int> out;
    out.reserve(chain.size());
    for (int i = 0; i < static_cast<int>(chain.size()); ++i) {
        const ChainOption& o = chain[i];
        const bool two_sided = std::isfinite(o.bid) && std::isfinite(o.ask);
        const double mid = screen_mid(o);

        if (f.require_two_sided && !(two_sided && o.bid > 0.0 && o.ask > 0.0)) continue;
        if (f.min_volume && !(o.volume >= *f.min_volume)) continue;
        if (f.min_oi && !(o.oi >= *f.min_oi)) continue;
        if (f.min_price && !(std::max(mid, 0.0) >= *f.min_price)) continue;
        if (f.expiry && o.expiry != *f.expiry) continue;
        if (!f.days_to_expiry.admits(o.years * 365.0)) continue;
        if (f.volume_ratio.on && !(o.oi > 0.0 && f.volume_ratio.admits(o.volume / o.oi))) continue;
        if (f.max_bid_ask_spread && !(two_sided && std::abs(o.ask - o.bid) <= *f.max_bid_ask_spread)) continue;
        if (f.max_bid_ask_spread_pct
            && !(two_sided && mid > 0.0 && (o.ask - o.bid) / mid <= *f.max_bid_ask_spread_pct)) continue;
        if (f.moneyness.on && !(o.forward > 0.0 && f.moneyness.admits(o.strike / o.forward))) continue;

        out.push_back(i);
    }
    return out;
}

void screen_compute_greeks(std::span<ChainOption> chain, std::span<const int> indices, double rate) {
    const int n = static_cast<int>(indices.size());
#pragma omp parallel for schedule(static)
    for (int k = 0; k < n; ++k) {
        ChainOption& o = chain[indices[k]];
        if (o.forward <= 0.0 || o.strike <= 0.0 || o.iv <= 0.0 || o.years <= 0.0) {
            o.delta = o.gamma = o.theta = o.vega = kNaN; // none: null in the answer
            continue;
        }
        black76_greeks(o.forward, o.strike, rate, o.iv, o.years, o.is_call,
                       &o.delta, &o.gamma, &o.theta, &o.vega);
    }
}

}  // namespace quant
