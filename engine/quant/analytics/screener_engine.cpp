#include "engine.h"
#include "quant/kernel.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <omp.h>

namespace quant::analytics {

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

const char* kind_name(ScreenKind k) {
    switch (k) {
        case ScreenKind::Single:     return "single";
        case ScreenKind::IronCondor: return "iron_condor";
        case ScreenKind::Straddle:   return "straddle";
        case ScreenKind::Strangle:   return "strangle";
        case ScreenKind::ForwardVol: return "forward_vol";
    }
    return "unknown";
}

std::string fmt_strike(double k) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%g", k);
    return buf;
}

std::string make_label(const StrategyRecord& s, std::span<const ChainOption> chain) {
    const auto& o = [&](int k) -> const ChainOption& { return chain[s.legs[k].option_index]; };
    const std::string dir = s.long_direction ? "LONG" : "SHORT";
    switch (s.kind) {
        case ScreenKind::Single:
            return std::string(s.legs[0].qty > 0 ? "BUY " : "SELL ") + (o(0).is_call ? "C" : "P")
                   + fmt_strike(o(0).strike) + " " + o(0).expiry;
        case ScreenKind::IronCondor:
            return "IC " + dir + " C" + fmt_strike(o(0).strike) + "/" + fmt_strike(o(1).strike)
                   + " P" + fmt_strike(o(2).strike) + "/" + fmt_strike(o(3).strike) + " " + o(0).expiry;
        case ScreenKind::Straddle:
            return "Straddle " + dir + " " + fmt_strike(o(0).strike) + " " + o(0).expiry;
        case ScreenKind::Strangle:
            return "Strangle " + dir + " C" + fmt_strike(o(0).strike) + " P" + fmt_strike(o(1).strike)
                   + " " + o(0).expiry;
        case ScreenKind::ForwardVol:
            return "FwdVol " + dir + " " + (o(0).is_call ? "C" : "P") + fmt_strike(o(0).strike) + " "
                   + o(0).expiry + "->" + o(1).expiry;
    }
    return "";
}

std::vector<double> model_prices(const ScreenerParams& p, std::span<const int> idx) {
    std::vector<double> px;
    if (p.model_vol == "none") return px;
    px.assign(p.chain.size(), kNaN);
    const int n = static_cast<int>(idx.size());
    const bool heston = p.model_vol == "heston";

#pragma omp parallel for schedule(dynamic, 16)
    for (int k = 0; k < n; ++k) {
        const ChainOption& o = p.chain[idx[k]];
        if (heston) {
            if (o.years <= 0.0 || o.forward <= 0.0) continue;
            const double df   = std::exp(-p.rate * o.years);
            const double call = heston_call_price(o.forward * df, o.strike, p.rate, o.years,
                                                  p.heston.v0, p.heston.kappa, p.heston.theta,
                                                  p.heston.xi, p.heston.rho);
            px[idx[k]] = o.is_call ? call : call - df * (o.forward - o.strike);   // put-call parity
            continue;
        }
        double vol = o.iv;
        if (p.model_vol == "flat") {
            vol = p.model_vol_flat;
        } else if (p.model_vol == "surface") {
            vol = vol_surface_interp(p.surface_strikes, p.surface_expiries, p.surface_ivs,
                                     p.spot, o.strike, o.years);
        }
        px[idx[k]] = vol > 0.0 ? black76_price(o.forward, o.strike, p.rate, vol, o.years, o.is_call) : kNaN;
    }
    return px;
}

ScreenedStrategy to_output(const StrategyRecord& s, const ScreenContext& c) {
    ScreenedStrategy out;
    out.kind      = kind_name(s.kind);
    out.direction = s.long_direction ? "LONG" : "SHORT";
    out.label     = make_label(s, c.chain);
    for (int k = 0; k < s.n_legs; ++k) {
        const ScreenLeg&   leg = s.legs[k];
        const ChainOption& o   = c.chain[leg.option_index];
        out.legs.push_back({
            .symbol      = o.symbol,
            .option_type = o.is_call ? "call" : "put",
            .expiry      = o.expiry,
            .strike      = o.strike,
            .years       = o.years,
            .forward     = o.forward,
            .qty         = leg.qty,
            .fill_price  = screen_fill_price(o, leg.qty, c.price_mode),
            .mark        = o.mark,
            .iv          = o.iv,
            .model_price = c.model_px.empty() ? kNaN : c.model_px[leg.option_index],
            .delta = o.delta, .gamma = o.gamma, .theta = o.theta, .vega = o.vega,
        });
    }
    const ScreenMetrics& m = s.m;
    out.debit = m.debit;         out.credit = m.credit;       out.cost = m.cost;
    out.max_gain = m.max_gain;   out.max_loss = m.max_loss;   out.rr = m.rr;
    out.net_delta = m.net_delta; out.net_gamma = m.net_gamma;
    out.net_theta = m.net_theta; out.net_vega = m.net_vega;
    out.avg_iv = m.avg_iv;       out.model_value = m.model_value;
    out.edge = m.edge;           out.forward_vol = m.forward_vol;
    return out;
}

}  // namespace

ScreenerResult run_screener(ScreenerParams p) {
    const auto t0 = std::chrono::steady_clock::now();
    ScreenerResult r;
    r.n_options_in = static_cast<int>(p.chain.size());

    const std::vector<int> idx = screen_filter_options(p.chain, p.option_filter);
    r.n_options_after_filter = static_cast<int>(idx.size());
    if (p.compute_greeks) screen_compute_greeks(p.chain, idx, p.rate);
    const std::vector<double> model_px = model_prices(p, idx);

    const ScreenContext ctx{
        .chain      = p.chain,
        .model_px   = model_px,
        .multiplier = p.multiplier,
        .price_mode = p.price_mode,
        .filter     = &p.strategy_filter,
        .rank       = p.rank,
    };

    using Generator = void (*)(const ScreenContext&, std::span<const int>, ScreenTopN&, ScreenCounts&);
    const std::pair<bool, std::pair<ScreenKind, Generator>> plan[] = {
        {p.strategies.single_calls, {ScreenKind::Single,     screen_generate_single_calls}},
        {p.strategies.iron_condors, {ScreenKind::IronCondor, screen_generate_iron_condors}},
        {p.strategies.straddles,    {ScreenKind::Straddle,   screen_generate_straddles}},
        {p.strategies.strangles,    {ScreenKind::Strangle,   screen_generate_strangles}},
        {p.strategies.forward_vols, {ScreenKind::ForwardVol, screen_generate_forward_vols}},
    };

    ScreenTopN top(p.rank);
    for (const auto& [enabled, entry] : plan) {
        if (!enabled) continue;
        ScreenTopN   kind_top(p.rank);
        ScreenCounts counts;
        entry.second(ctx, idx, kind_top, counts);
        top.merge(std::move(kind_top));
        r.by_kind.push_back({kind_name(entry.first), counts.generated, counts.passed});
        r.n_generated += counts.generated;
        r.n_passed    += counts.passed;
    }

    for (const auto& s : top.finish()) r.top.push_back(to_output(s, ctx));
    r.runtime_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return r;
}

}  // namespace quant::analytics
