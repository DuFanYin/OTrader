#include "engine.h"
#include "quant/kernel.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <ranges>
#include <string>
#include <vector>
#include <omp.h>

namespace quant::analytics {

namespace {

double quantile_sorted(const std::vector<double>& sv, double q) {
    if (sv.empty()) return 0.0;
    const double pos = q * (sv.size() - 1);
    const int lo = static_cast<int>(pos);
    const int hi = std::min(lo + 1, static_cast<int>(sv.size()) - 1);
    return sv[lo] * (1.0 - (pos - lo)) + sv[hi] * (pos - lo);
}

double pop_std(const std::vector<double>& v) {
    if (v.empty()) return 0.0;
    const double m = std::accumulate(v.begin(), v.end(), 0.0) / v.size();
    double acc = 0.0;
    for (double x : v) acc += (x - m) * (x - m);
    return std::sqrt(acc / v.size());
}

MethodStats agg_method(const std::string& name, const std::vector<double>& vals,
                        const std::vector<double>* errs = nullptr) {
    MethodStats s;
    s.method = name;
    const int n = static_cast<int>(vals.size());
    if (n == 0) return s;
    s.avg = std::accumulate(vals.begin(), vals.end(), 0.0) / n;
    s.min = *std::ranges::min_element(vals);
    s.max = *std::ranges::max_element(vals);
    double var = 0.0;
    for (double v : vals) var += (v - s.avg) * (v - s.avg);
    s.std = std::sqrt(var / n);
    if (errs && !errs->empty())
        s.avg_err_vs_bs = std::accumulate(errs->begin(), errs->end(), 0.0) / n;
    return s;
}

PricingBatchResult assemble_batch(
    const std::vector<double>& spots,
    const std::vector<double>& strikes,
    const std::vector<double>& vols,
    const std::vector<double>& out_mc,
    const std::vector<double>& out_bs,
    const std::vector<double>& out_bin,
    double runtime_ms
) {
    const int n = static_cast<int>(spots.size());
    PricingBatchResult r;
    r.job_count  = n;
    r.runtime_ms = runtime_ms;

    std::vector<double> spreads(n), mc_err(n), bin_err(n);
    for (int i = 0; i < n; ++i) {
        spreads[i] = std::max({out_mc[i], out_bs[i], out_bin[i]})
                   - std::min({out_mc[i], out_bs[i], out_bin[i]});
        mc_err[i]  = out_mc[i]  - out_bs[i];
        bin_err[i] = out_bin[i] - out_bs[i];
        BatchJobResult row;
        row.spot    = spots[i];
        row.strike  = strikes[i];
        row.vol     = vols[i];
        row.mc      = out_mc[i];
        row.bs      = out_bs[i];
        row.binomial = out_bin[i];
        row.spread  = spreads[i];
        r.rows.push_back(row);
    }

    auto sorted = spreads;
    std::ranges::sort(sorted);
    const double sum = std::accumulate(spreads.begin(), spreads.end(), 0.0);
    const double mean = n > 0 ? sum / n : 0.0;
    r.avg_spread = mean;
    r.p50_spread = quantile_sorted(sorted, 0.50);
    r.p95_spread = quantile_sorted(sorted, 0.95);
    r.max_spread = sorted.empty() ? 0.0 : sorted.back();
    r.min_spread = sorted.empty() ? 0.0 : sorted.front();
    r.spread_std = pop_std(spreads);
    r.spread_cv  = r.spread_std / std::max(std::abs(mean), 1e-12);
    r.worst_idx  = -1; r.best_idx = -1;
    for (int i = 0; i < n; ++i) {
        if (r.worst_idx < 0 || spreads[i] > spreads[r.worst_idx]) r.worst_idx = i;
        if (r.best_idx  < 0 || spreads[i] < spreads[r.best_idx])  r.best_idx  = i;
    }

    r.method_summary.push_back(agg_method("bs",       out_bs,  nullptr));
    r.method_summary.push_back(agg_method("mc",       out_mc,  &mc_err));
    r.method_summary.push_back(agg_method("binomial", out_bin, &bin_err));
    return r;
}

}  // namespace

PricingResult run_pricing(const PricingParams& p) {
    const int steps = p.n_steps > 0 ? p.n_steps : std::max(10, static_cast<int>(p.maturity * 250.0));
    PricingResult r;
    r.numeraire = p.numeraire;

    if (p.product_type == "digital_call") {
        r.bs = digital_call_bs_price(p.spot, p.strike, p.rate, p.vol, p.maturity, p.dividend_yield);
        r.mc = r.bs; r.binomial = r.bs;
    } else {
        const double q     = p.dividend_yield;
        const double r_eff = p.rate - q;
        const bool is_call = p.option_type.empty() ? (p.product_type != "european_put") : (p.option_type != "put");
        r.is_call = is_call;
        SamplerType sampler = SamplerType::Pseudorandom;
        if (p.mc_sampler == "antithetic") sampler = SamplerType::Antithetic;
        else if (p.mc_sampler == "sobol") sampler = SamplerType::Sobol;
        // The BS / MC / binomial kernels take a single rate. Run them at r - q so
        // the forward is right, then rescale: discounting at r - q instead of r
        // overstates every European price by exactly exp(qT).
        const double q_disc = std::exp(-q * p.maturity);
        r.mc       = q_disc * mc_price_with_stderr(p.spot, p.strike, r_eff, p.vol, p.maturity, p.n_paths, &r.mc_std_err, is_call, sampler);
        r.mc_std_err *= q_disc;
        r.bs       = q_disc * bs_closed_form_price(p.spot, p.strike, r_eff, p.vol, p.maturity, is_call);
        r.binomial = q_disc * binomial_price(p.spot, p.strike, r_eff, p.vol, p.maturity, steps, is_call);
        r.trinomial = trinomial_price(p.spot, p.strike, p.rate, q, p.vol, p.maturity, steps, is_call, false);
        pricing_greeks(p.spot, p.strike, p.rate, p.vol, p.maturity, q,
                       &r.delta_bs, &r.gamma_bs, &r.theta_bs, &r.vega_bs, &r.rho_bs);
        if (!is_call) {
            // Put greeks from the call greeks via put-call parity.
            const double df_r = std::exp(-p.rate * p.maturity);
            r.delta_bs -= q_disc;
            r.theta_bs += p.rate * p.strike * df_r - q * p.spot * q_disc;
            r.rho_bs   -= p.strike * p.maturity * df_r;
        }
        pricing_error_decomp(r.mc, r.bs, r.binomial, &r.mc_minus_bs, &r.binomial_minus_bs);
        if (p.is_american) {
            r.american_binomial  = binomial_crr_price(p.spot, p.strike, p.rate, q, p.vol, p.maturity, steps, is_call, true);
            r.american_trinomial = trinomial_price(p.spot, p.strike, p.rate, q, p.vol, p.maturity, steps, is_call, true);
            PdeSpec spec;
            spec.spot = p.spot; spec.strike = p.strike; spec.rate = p.rate; spec.dividend_yield = q;
            spec.vol = p.vol; spec.maturity = p.maturity; spec.s_steps = 200; spec.t_steps = std::max(200, steps);
            spec.method = 0; spec.is_call = is_call; spec.american = true;
            const PdeOutcome pde = pde_solve(spec);
            r.american_pde = pde.price;
            r.american_pde_psor_iterations = pde.psor_iterations;
            const int lsm_paths = p.lsm_paths > 0 ? p.lsm_paths : std::clamp(p.n_paths, 1000, 200000);
            const LsmResult lsm = lsm_american_price(p.spot, p.strike, p.rate, q, p.vol, p.maturity,
                                                     lsm_paths, std::max(p.lsm_steps, 1), is_call);
            r.american_lsm        = lsm.price;
            r.american_lsm_stderr = lsm.std_error;
            r.american_lsm_paths  = lsm.n_paths;
            r.american     = r.american_binomial;
            r.has_american = true;
        }
    }

    if (p.fx_mode) {
        const double scale = 1.0 / std::max(p.spot, 1e-6);
        r.mc *= scale; r.bs *= scale; r.binomial *= scale; r.trinomial *= scale;
        if (r.has_american) {
            r.american *= scale; r.american_binomial *= scale; r.american_trinomial *= scale;
            r.american_pde *= scale; r.american_lsm *= scale; r.american_lsm_stderr *= scale;
        }
    }

    r.spread     = std::max({r.mc, r.bs, r.binomial}) - std::min({r.mc, r.bs, r.binomial});
    r.rel_spread = r.spread / std::max(std::abs(r.bs), 1e-12);
    r.stability  = (r.spread < std::max(1e-6, 0.05 * std::max(std::abs(r.bs), 1.0)))
                   ? "good" : "check_model_params";
    r.in_spot     = p.spot;
    r.in_strike   = p.strike;
    r.in_vol      = p.vol;
    r.in_maturity = p.maturity;
    r.in_rate     = p.rate;
    r.in_n_paths  = p.n_paths;
    return r;
}

PricingBatchResult run_pricing_batch(const std::vector<PricingParams>& jobs, double runtime_ms) {
    const int n = static_cast<int>(jobs.size());
    std::vector<double> spots(n), strikes(n), rates(n), vols(n), mats(n), divs(n);
    std::vector<int>    npaths(n);
    for (int i = 0; i < n; ++i) {
        spots[i]   = jobs[i].spot;
        strikes[i] = jobs[i].strike;
        rates[i]   = jobs[i].rate;
        vols[i]    = jobs[i].vol;
        mats[i]    = jobs[i].maturity;
        divs[i]    = jobs[i].dividend_yield;
        npaths[i]  = jobs[i].n_paths;
    }
    std::vector<double> out_mc(n), out_bs(n), out_bin(n);
    pricing_batch(spots, strikes, rates, vols, mats, npaths, divs, out_mc, out_bs, out_bin);

    std::vector<double> final_vols(n);
    for (int i = 0; i < n; ++i) final_vols[i] = jobs[i].vol;
    return assemble_batch(spots, strikes, final_vols, out_mc, out_bs, out_bin, runtime_ms);
}

PricingBatchResult run_pricing_batch_grid(const BatchGridParams& p, double runtime_ms) {
    const int n = std::max(1, std::min(p.n_jobs, 500));
    const double center = (n - 1) / 2.0;
    std::vector<double> spots(n), strikes(n), rates(n), vols(n), mats(n), divs(n);
    std::vector<int>    npaths(n);
    for (int i = 0; i < n; ++i) {
        const double k = static_cast<double>(i) - center;
        spots[i]   = p.base.spot   * std::max(0.01, 1.0 + k * p.spot_shock);
        vols[i]    = p.base.vol    * std::max(0.05, 1.0 + k * p.spot_shock * 0.5);
        strikes[i] = p.base.strike;
        rates[i]   = p.base.rate;
        mats[i]    = p.base.maturity;
        divs[i]    = p.base.dividend_yield;
        npaths[i]  = p.base.n_paths;
    }
    std::vector<double> out_mc(n), out_bs(n), out_bin(n);
    pricing_batch(spots, strikes, rates, vols, mats, npaths, divs, out_mc, out_bs, out_bin);
    return assemble_batch(spots, strikes, vols, out_mc, out_bs, out_bin, runtime_ms);
}

namespace {

// Legs ordered short call, long call, short put, long put (or all signs flipped),
// one expiry, call strikes above put strikes — the shape the screener emits.
bool is_iron_condor(const std::vector<LegSpec>& legs) {
    const auto& sc = legs[0]; const auto& lc = legs[1];
    const auto& sp = legs[2]; const auto& lp = legs[3];
    if (sc.option_type != "call" || lc.option_type != "call"
        || sp.option_type != "put" || lp.option_type != "put") return false;
    const double sign = sc.quantity;
    if (sign == 0.0 || lc.quantity * sign >= 0 || sp.quantity * sign <= 0 || lp.quantity * sign >= 0) return false;
    if (sc.maturity != lc.maturity || sc.maturity != sp.maturity || sc.maturity != lp.maturity) return false;
    return lp.strike < sp.strike && sp.strike < sc.strike && sc.strike < lc.strike;
}

}  // namespace

MultiLegResult run_multi_leg(const MultiLegParams& p) {
    const double r_eff = p.rate - p.dividend_yield;

    MultiLegResult r;
    for (const auto& leg : p.legs) {
        const bool is_call = (leg.option_type != "put");
        const double vol = leg.vol      > 0.0 ? leg.vol      : p.vol;
        const double t   = leg.maturity > 0.0 ? leg.maturity : p.maturity;
        // A leg with its own forward is priced on the spot that carries to that
        // forward at r_eff, i.e. Black-76 on the forward.
        const double s   = leg.forward  > 0.0 ? leg.forward * std::exp(-r_eff * t) : p.spot;
        LegResult lr;
        lr.option_type = leg.option_type;
        lr.strike      = leg.strike;
        lr.quantity    = leg.quantity;
        lr.vol         = vol;
        lr.maturity    = t;
        lr.forward     = leg.forward > 0.0 ? leg.forward : 0.0;
        // Kernels run at r - q for the forward; rescale the r - q discount to r.
        const double q_disc = std::exp(-p.dividend_yield * t);
        lr.bs_price    = q_disc * bs_closed_form_price(s, leg.strike, r_eff, vol, t, is_call);
        lr.mc_price    = q_disc * mc_price_full(s, leg.strike, r_eff, vol, t, p.n_paths, is_call);
        lr.binomial_price = q_disc * binomial_price(s, leg.strike, r_eff, vol, t, std::max(10, static_cast<int>(t * 250.0)), is_call);

        double delta = 0.0, gamma = 0.0, theta = 0.0, vega = 0.0, rho = 0.0;
        pricing_greeks(s, leg.strike, p.rate, vol, t, p.dividend_yield,
                       &delta, &gamma, &theta, &vega, &rho);
        if (!is_call) {
            // put delta = call delta - exp(-q*T)
            delta -= std::exp(-p.dividend_yield * t);
        }
        lr.delta_bs = delta;
        lr.vega_bs  = vega;

        r.net_bs_price += leg.quantity * lr.bs_price;
        r.net_mc_price += leg.quantity * lr.mc_price;
        r.net_binomial_price += leg.quantity * lr.binomial_price;
        r.net_delta    += leg.quantity * lr.delta_bs;
        r.net_vega     += leg.quantity * lr.vega_bs;
        r.legs.push_back(lr);
    }

    const int n = static_cast<int>(p.legs.size());
    const auto leg_t = [&](const LegSpec& l) { return l.maturity > 0.0 ? l.maturity : p.maturity; };
    if (n == 2 && p.legs[0].option_type == p.legs[1].option_type
        && std::abs(p.legs[0].strike - p.legs[1].strike) < 1e-6
        && std::abs(leg_t(p.legs[0]) - leg_t(p.legs[1])) > 1e-9
        && p.legs[0].quantity * p.legs[1].quantity < 0) {
        r.strategy_hint = "calendar";
    } else if (n == 4 && is_iron_condor(p.legs)) {
        r.strategy_hint = p.legs[0].quantity < 0 ? "iron_condor" : "reverse_iron_condor";
    } else if (n == 2) {
        const bool l0_call = (p.legs[0].option_type != "put");
        const bool l1_call = (p.legs[1].option_type != "put");
        const double q0 = p.legs[0].quantity, q1 = p.legs[1].quantity;
        if (l0_call && l1_call && q0 > 0 && q1 < 0)
            r.strategy_hint = "bull_call_spread";
        else if (!l0_call && !l1_call && q0 > 0 && q1 < 0)
            r.strategy_hint = "bear_put_spread";
        else if (l0_call != l1_call && std::abs(p.legs[0].strike - p.legs[1].strike) < 1e-6)
            r.strategy_hint = "straddle";
        else if (l0_call && !l1_call && q0 > 0 && q1 > 0)
            r.strategy_hint = "strangle";
        else
            r.strategy_hint = "custom_2leg";
    } else if (n == 3) {
        r.strategy_hint = "butterfly_or_custom";
    } else if (n > 3) {
        r.strategy_hint = "condor_or_custom";
    } else if (n == 1) {
        r.strategy_hint = p.legs[0].option_type == "put" ? "long_put" : "long_call";
    }

    return r;
}

GreekSurfaceResult run_greek_surface(const GreekSurfaceParams& p) {
    const int ns = std::max(2, std::min(p.n_spots, 50));
    const int nt = std::max(2, std::min(p.n_mats,  30));
    const double s_min = std::max(1e-6, p.spot_min);
    const double s_max = std::max(s_min + 1e-6, p.spot_max);
    const double m_min = std::max(1e-6, p.mat_min);
    const double m_max = std::max(m_min + 1e-6, p.mat_max);

    std::vector<double> spots(ns), mats(nt);
    for (int i = 0; i < ns; ++i)
        spots[i] = s_min + (s_max - s_min) * i / (ns - 1);
    for (int j = 0; j < nt; ++j)
        mats[j]  = m_min + (m_max - m_min) * j / (nt - 1);

    const int greek_code = [&]() -> int {
        if (p.greek == "gamma") return 1;
        if (p.greek == "vega")  return 2;
        if (p.greek == "theta") return 3;
        if (p.greek == "rho")   return 4;
        return 0; // delta default
    }();

    std::vector<double> grid(ns * nt, 0.0);
    greek_surface_grid(p.strike, p.rate, p.vol, p.dividend_yield,
                       spots, mats, greek_code, grid);

    GreekSurfaceResult r;
    r.greek_name = p.greek;
    r.spots      = spots;
    r.maturities = mats;
    r.grid       = grid;
    r.strike     = p.strike;
    r.vol        = p.vol;
    r.rate       = p.rate;
    if (!grid.empty()) {
        r.grid_min = *std::ranges::min_element(grid);
        r.grid_max = *std::ranges::max_element(grid);
    }
    return r;
}

}  // namespace quant::analytics
