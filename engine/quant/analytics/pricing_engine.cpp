// Pricing tasks: one option by every method, batches of calls, a position of legs, a Greek's
// surface, the lattices on a ladder of step counts, Black on forwards.
#include "json_io.h"
#include "quant/kernel.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <ranges>
#include <string>
#include <vector>

namespace quant {

void greek_surface_grid(
    double strike, double rate, double vol, double dividend_yield,
    std::span<const double> spots,
    std::span<const double> maturities,
    int greek_code,
    std::span<double> out_grid
) {
    const int n_s = static_cast<int>(spots.size());
    const int n_t = static_cast<int>(maturities.size());
    if (n_s == 0 || n_t == 0 || static_cast<int>(out_grid.size()) < n_s * n_t) return;

#pragma omp parallel for schedule(static)
    for (int i = 0; i < n_s; ++i) {
        for (int j = 0; j < n_t; ++j) {
            double g[5];   // delta, gamma, vega, theta, rho: greek_code's order
            pricing_greeks(true, spots[i], strike, rate, dividend_yield, vol, maturities[j],
                           &g[0], &g[1], &g[3], &g[2], &g[4]);
            out_grid[i * n_t + j] = greek_code >= 0 && greek_code < 5 ? g[greek_code] : 0.0;
        }
    }
}

void pricing_batch(
    std::span<const double> spots,
    std::span<const double> strikes,
    std::span<const double> rates,
    std::span<const double> vols,
    std::span<const double> maturities,
    std::span<const int> n_paths_arr,
    std::span<const double> div_yields,
    std::span<double> out_mc,
    std::span<double> out_bs,
    std::span<double> out_binomial
) {
    const size_t n_jobs = spots.size();
    if (n_jobs == 0) return;
    if (strikes.size() != n_jobs || rates.size() != n_jobs || vols.size() != n_jobs ||
        maturities.size() != n_jobs || n_paths_arr.size() != n_jobs ||
        div_yields.size() != n_jobs ||
        out_mc.size() != n_jobs || out_bs.size() != n_jobs || out_binomial.size() != n_jobs) return;

#pragma omp parallel for schedule(dynamic)
    for (int i = 0; i < static_cast<int>(n_jobs); ++i) {
        const double s = spots[i], k = strikes[i], r = rates[i], q = div_yields[i];
        const double v = vols[i], t = maturities[i];
        const int steps = std::max(10, static_cast<int>(t * 250));
        out_mc[i]       = mc_price(s, k, r, q, v, t, n_paths_arr[i], true);
        out_bs[i]       = black_price(true, spot_forward(s, r, q, t), k, t, r, v);
        out_binomial[i] = binomial_crr_price(s, k, r, q, v, t, steps, true, false);
    }
}

}  // namespace quant

namespace quant::analytics {

namespace {

constexpr double kZ95 = 1.96;
constexpr std::size_t kMaxBatchJobs = 500;

double pop_std(const std::vector<double>& v) {
    if (v.empty()) return 0.0;
    const double m = std::accumulate(v.begin(), v.end(), 0.0) / v.size();
    double acc = 0.0;
    for (double x : v) acc += (x - m) * (x - m);
    return std::sqrt(acc / v.size());
}

json method_stats(const std::string& name, const std::vector<double>& vals, const std::vector<double>* errs) {
    const int n = static_cast<int>(vals.size());
    double avg = 0.0, min = 0.0, max = 0.0, sd = 0.0, err = 0.0;
    if (n > 0) {
        avg = std::accumulate(vals.begin(), vals.end(), 0.0) / n;
        min = *std::ranges::min_element(vals);
        max = *std::ranges::max_element(vals);
        double var = 0.0;
        for (double v : vals) var += (v - avg) * (v - avg);
        sd = std::sqrt(var / n);
        if (errs && !errs->empty()) err = std::accumulate(errs->begin(), errs->end(), 0.0) / n;
    }
    return {{"method", name}, {"avg", avg}, {"min", min}, {"max", max}, {"std", sd}, {"avg_err_vs_bs", err}};
}

// One option's inputs, as pricing and each batch job read them.
struct Option {
    double spot, strike, rate, vol, maturity, dividend_yield;
    int n_paths;
};

Option read_option(const json& j) {
    return {jv(j, "spot", 0.0), jv(j, "strike", 0.0), jv(j, "rate", 0.0), jv(j, "vol", 0.0),
            jv(j, "maturity", 0.0), jv(j, "dividend_yield", 0.0), jv(j, "n_paths", 10000)};
}

// Calls by Monte Carlo, Black-Scholes and a CRR tree, with the spread between the three per job.
std::string batch_answer(const std::vector<Option>& jobs, double t0_ms) {
    const int n = static_cast<int>(jobs.size());
    std::vector<double> spots(n), strikes(n), rates(n), vols(n), mats(n), divs(n);
    std::vector<int> npaths(n);
    for (int i = 0; i < n; ++i) {
        spots[i] = jobs[i].spot;
        strikes[i] = jobs[i].strike;
        rates[i] = jobs[i].rate;
        vols[i] = jobs[i].vol;
        mats[i] = jobs[i].maturity;
        divs[i] = jobs[i].dividend_yield;
        npaths[i] = jobs[i].n_paths;
    }
    std::vector<double> mc(n), bs(n), bin(n);
    pricing_batch(spots, strikes, rates, vols, mats, npaths, divs, mc, bs, bin);

    std::vector<double> spreads(n), mc_err(n), bin_err(n);
    json rows = json::array();
    for (int i = 0; i < n; ++i) {
        spreads[i] = std::max({mc[i], bs[i], bin[i]}) - std::min({mc[i], bs[i], bin[i]});
        mc_err[i] = mc[i] - bs[i];
        bin_err[i] = bin[i] - bs[i];
        rows.push_back({{"#", i}, {"spot", spots[i]}, {"strike", strikes[i]}, {"vol", vols[i]}, {"bs", bs[i]},
                        {"mc", mc[i]}, {"binomial", bin[i]}, {"mc-bs", mc_err[i]}, {"bin-bs", bin_err[i]},
                        {"spread", spreads[i]}});
    }
    auto sorted = spreads;
    std::ranges::sort(sorted);
    const double mean = n > 0 ? std::accumulate(spreads.begin(), spreads.end(), 0.0) / n : 0.0;
    const double sd = pop_std(spreads);
    int worst = -1, best = -1;
    for (int i = 0; i < n; ++i) {
        if (worst < 0 || spreads[i] > spreads[worst]) worst = i;
        if (best < 0 || spreads[i] < spreads[best]) best = i;
    }
    const double ms = elapsed_ms(t0_ms);
    json summary = {
        {"job_count", n},
        {"total_compute_ms", ms},
        {"avg_method_spread", mean},
        {"p50_method_spread", interp_quantile_sorted(sorted, 0.50)},
        {"p95_method_spread", interp_quantile_sorted(sorted, 0.95)},
        {"max_method_spread", sorted.empty() ? 0.0 : sorted.back()},
        {"min_method_spread", sorted.empty() ? 0.0 : sorted.front()},
        {"avg_runtime_per_job_ms", n > 0 ? ms / n : 0.0},
        {"worst_spread_job_index", worst},
        {"best_spread_job_index", best},
        {"spread_std", sd},
        {"spread_cv", sd / std::max(std::abs(mean), 1e-12)},
    };
    json methods = {method_stats("bs", bs, nullptr), method_stats("mc", mc, &mc_err),
                    method_stats("binomial", bin, &bin_err)};
    return ok_response(std::move(summary), {{"flat_rows", std::move(rows)}, {"method_summary", std::move(methods)}},
                       t0_ms);
}

// Legs ordered short call, long call, short put, long put (or all signs flipped),
// one expiry, call strikes above put strikes — the shape the screener emits.
bool is_iron_condor(const std::vector<LegSpec>& legs) {
    const auto& sc = legs[0];
    const auto& lc = legs[1];
    const auto& sp = legs[2];
    const auto& lp = legs[3];
    if (sc.option_type != "call" || lc.option_type != "call" || sp.option_type != "put" || lp.option_type != "put")
        return false;
    const double sign = sc.quantity;
    if (sign == 0.0 || lc.quantity * sign >= 0 || sp.quantity * sign <= 0 || lp.quantity * sign >= 0) return false;
    if (sc.maturity != lc.maturity || sc.maturity != sp.maturity || sc.maturity != lp.maturity) return false;
    return lp.strike < sp.strike && sp.strike < sc.strike && sc.strike < lc.strike;
}

std::string strategy_hint(const std::vector<LegSpec>& legs, double maturity) {
    const int n = static_cast<int>(legs.size());
    const auto leg_t = [&](const LegSpec& l) { return l.maturity > 0.0 ? l.maturity : maturity; };
    if (n == 2 && legs[0].option_type == legs[1].option_type && std::abs(legs[0].strike - legs[1].strike) < 1e-6 &&
        std::abs(leg_t(legs[0]) - leg_t(legs[1])) > 1e-9 && legs[0].quantity * legs[1].quantity < 0)
        return "calendar";
    if (n == 4 && is_iron_condor(legs)) return legs[0].quantity < 0 ? "iron_condor" : "reverse_iron_condor";
    if (n == 2) {
        const bool l0_call = legs[0].option_type != "put";
        const bool l1_call = legs[1].option_type != "put";
        const double q0 = legs[0].quantity, q1 = legs[1].quantity;
        if (l0_call && l1_call && q0 > 0 && q1 < 0) return "bull_call_spread";
        if (!l0_call && !l1_call && q0 > 0 && q1 < 0) return "bear_put_spread";
        if (l0_call != l1_call && std::abs(legs[0].strike - legs[1].strike) < 1e-6) return "straddle";
        if (l0_call && !l1_call && q0 > 0 && q1 > 0) return "strangle";
        return "custom_2leg";
    }
    if (n == 3) return "butterfly_or_custom";
    if (n > 3) return "condor_or_custom";
    if (n == 1) return legs[0].option_type == "put" ? "long_put" : "long_call";
    return "";
}

}  // namespace

std::string run_pricing_json(const std::string& src, double t0_ms) {
    const auto req = parse_request(src, {"spot", "strike", "rate", "vol", "maturity"},
                                   "missing_required_pricing_fields");
    if (!req) return req.error();
    const json& j = *req;
    const Option o = read_option(j);
    const int n_steps = jv(j, "n_steps", 0);
    const bool is_american = jv(j, "is_american", false);
    const auto product_type = jv<std::string>(j, "product_type", "european_call");
    const auto mc_sampler = jv<std::string>(j, "mc_sampler", "pseudorandom");
    const auto option_type = jv<std::string>(j, "option_type", "");
    const int lsm_paths_given = jv(j, "lsm_paths", 0);
    const int lsm_steps = jv(j, "lsm_steps", 50);
    const double s = o.spot, k = o.strike, r = o.rate, q = o.dividend_yield, v = o.vol, t = o.maturity;
    const int steps = n_steps > 0 ? n_steps : std::max(10, static_cast<int>(t * 250.0));

    bool is_call = true;
    double mc = 0.0, bs = 0.0, binomial = 0.0, trinomial = 0.0, mc_std_err = 0.0;
    double delta = 0.0, gamma = 0.0, theta = 0.0, vega = 0.0, rho = 0.0;
    bool american = false;
    double am_binomial = 0.0, am_trinomial = 0.0, am_pde = 0.0, am_lsm = 0.0, am_lsm_err = 0.0;
    long long am_pde_iterations = 0;
    int am_lsm_paths = 0;
    if (product_type == "digital_call") {
        bs = black_digital_call(spot_forward(s, r, q, t), k, t, r, v);
        mc = bs;
        binomial = bs;
    } else {
        is_call = option_type.empty() ? product_type != "european_put" : option_type != "put";
        const SamplerType sampler = mc_sampler == "antithetic" ? SamplerType::Antithetic
                                    : mc_sampler == "sobol"    ? SamplerType::Sobol
                                                               : SamplerType::Pseudorandom;
        mc = mc_price(s, k, r, q, v, t, o.n_paths, is_call, sampler, &mc_std_err);
        bs = black_price(is_call, spot_forward(s, r, q, t), k, t, r, v);
        binomial = binomial_crr_price(s, k, r, q, v, t, steps, is_call, false);
        trinomial = trinomial_price(s, k, r, q, v, t, steps, is_call, false);
        pricing_greeks(is_call, s, k, r, q, v, t, &delta, &gamma, &theta, &vega, &rho);
        if (is_american) {
            american = true;
            am_binomial = binomial_crr_price(s, k, r, q, v, t, steps, is_call, true);
            am_trinomial = trinomial_price(s, k, r, q, v, t, steps, is_call, true);
            PdeSpec spec;
            spec.spot = s;
            spec.strike = k;
            spec.rate = r;
            spec.dividend_yield = q;
            spec.vol = v;
            spec.maturity = t;
            spec.s_steps = 200;
            spec.t_steps = std::max(200, steps);
            spec.method = 0;
            spec.is_call = is_call;
            spec.american = true;
            const PdeOutcome pde = pde_solve(spec);
            am_pde = pde.price;
            am_pde_iterations = pde.psor_iterations;
            const int lsm_paths = lsm_paths_given > 0 ? lsm_paths_given : std::clamp(o.n_paths, 1000, 200000);
            const LsmResult lsm = lsm_american_price(s, k, r, q, v, t, lsm_paths, std::max(lsm_steps, 1), is_call);
            am_lsm = lsm.price;
            am_lsm_err = lsm.std_error;
            am_lsm_paths = lsm.n_paths;
        }
    }
    // The gaps to Black-Scholes before the FX scaling.
    const double mc_minus_bs = product_type == "digital_call" ? 0.0 : mc - bs;
    const double binomial_minus_bs = product_type == "digital_call" ? 0.0 : binomial - bs;
    if (jv(j, "fx_mode", false)) {
        const double scale = 1.0 / std::max(s, 1e-6);
        for (double* x : {&mc, &bs, &binomial, &trinomial, &am_binomial, &am_trinomial, &am_pde, &am_lsm, &am_lsm_err})
            *x *= scale;
    }
    const double spread = std::max({mc, bs, binomial}) - std::min({mc, bs, binomial});
    json summary = {
        {"mc", mc},
        {"bs", bs},
        {"binomial", binomial},
        {"mc_std_err", mc_std_err},
        {"mc_ci_low", mc - kZ95 * mc_std_err},
        {"mc_ci_high", mc + kZ95 * mc_std_err},
        {"american", american ? json(am_binomial) : json(nullptr)},
        {"option_type", is_call ? "call" : "put"},
        {"trinomial", trinomial},
        {"trinomial_minus_bs", trinomial - bs},
        {"american_methods", american ? json{{"binomial", am_binomial},
                                             {"trinomial", am_trinomial},
                                             {"pde_psor", am_pde},
                                             {"pde_psor_iterations", am_pde_iterations},
                                             {"lsm", am_lsm},
                                             {"lsm_std_err", am_lsm_err},
                                             {"lsm_paths", am_lsm_paths},
                                             {"early_exercise_premium", am_binomial - binomial}}
                                      : json(nullptr)},
        {"method_spread", spread},
        {"relative_spread", spread / std::max(std::abs(bs), 1e-12)},
        {"pricing_stability",
         spread < std::max(1e-6, 0.05 * std::max(std::abs(bs), 1.0)) ? "good" : "check_model_params"},
        {"numeraire", jv<std::string>(j, "numeraire", "money_market")},
        {"greeks", {{"delta_bs", delta}, {"gamma_bs", gamma}, {"theta_bs", theta}, {"vega_bs", vega}, {"rho_bs", rho}}},
        {"error_decomposition", {{"mc_minus_bs", mc_minus_bs}, {"binomial_minus_bs", binomial_minus_bs}}},
        {"abs_mc_bs", std::abs(mc_minus_bs)},
        {"abs_binomial_bs", std::abs(binomial_minus_bs)},
    };
    return ok_response(std::move(summary), json::object(), t0_ms);
}

// A ladder of jobs around one option: the spot shocked by i * spot_shock, the vol by half that.
std::string run_pricing_batch_grid_json(const std::string& src, double t0_ms) {
    const auto req = parse_request(src, {"spot", "strike", "rate", "vol", "maturity"},
                                   "missing_required_pricing_batch_fields");
    if (!req) return req.error();
    const Option base = read_option(*req);
    const int n = std::max(1, std::min(jv(*req, "n_jobs", 16), static_cast<int>(kMaxBatchJobs)));
    const double shock = jv(*req, "spot_shock", 0.02);
    const double center = (n - 1) / 2.0;
    std::vector<Option> jobs(n, base);
    for (int i = 0; i < n; ++i) {
        const double k = static_cast<double>(i) - center;
        jobs[i].spot = base.spot * std::max(0.01, 1.0 + k * shock);
        jobs[i].vol = base.vol * std::max(0.05, 1.0 + k * shock * 0.5);
    }
    return batch_answer(jobs, t0_ms);
}

// {"jobs": [{"spot", "strike", "rate", "vol", "maturity", "dividend_yield", "n_paths"}, ...]}:
// calls.
std::string run_pricing_batch_json(const std::string& src, double t0_ms) {
    const auto req = parse_request(src, {"jobs"}, "missing_required_pricing_batch_fields");
    if (!req) return req.error();
    const json& given = (*req)["jobs"];
    if (!given.is_array() || given.empty() || given.size() > kMaxBatchJobs)
        return err_response("jobs: a list of 1 to 500 options");
    std::vector<Option> jobs;
    jobs.reserve(given.size());
    for (const json& job : given) {
        if (!job.is_object() || !has_fields(job, {"spot", "strike", "rate", "vol", "maturity"}))
            return err_response("missing_required_pricing_batch_fields");
        jobs.push_back(read_option(job));
    }
    return batch_answer(jobs, t0_ms);
}

std::string run_multi_leg_json(const std::string& src, double t0_ms) {
    const auto req = parse_request(src, {"spot", "rate", "vol", "maturity", "legs"},
                                   "missing_required_multi_leg_fields");
    if (!req) return req.error();
    const json& j = *req;
    const double spot = jv(j, "spot", 0.0), rate = jv(j, "rate", 0.0), vol = jv(j, "vol", 0.2);
    const double maturity = jv(j, "maturity", 1.0), q = jv(j, "dividend_yield", 0.0);
    const int n_paths = jv(j, "n_paths", 10000);
    const auto legs = parse_legs(j, spot);
    if (legs.empty()) return err_response("legs_array_empty");

    double net_bs = 0.0, net_mc = 0.0, net_bin = 0.0, net_delta = 0.0, net_vega = 0.0;
    json rows = json::array();
    for (const auto& leg : legs) {
        const bool is_call = leg.option_type != "put";
        const double v = leg.vol > 0.0 ? leg.vol : vol;
        const double t = leg.maturity > 0.0 ? leg.maturity : maturity;
        // A leg with its own forward is priced on the spot that carries to that forward at r - q,
        // i.e. Black-76 on the forward.
        const double s = leg.forward > 0.0 ? leg.forward * std::exp(-(rate - q) * t) : spot;
        const double forward = leg.forward > 0.0 ? leg.forward : spot_forward(s, rate, q, t);
        const double bs = black_price(is_call, forward, leg.strike, t, rate, v);
        const double mc = mc_price(s, leg.strike, rate, q, v, t, n_paths, is_call);
        const double bin = binomial_crr_price(s, leg.strike, rate, q, v, t, std::max(10, static_cast<int>(t * 250.0)),
                                              is_call, false);
        double delta = 0.0, gamma = 0.0, theta = 0.0, vega = 0.0, rho = 0.0;
        pricing_greeks(is_call, s, leg.strike, rate, q, v, t, &delta, &gamma, &theta, &vega, &rho);
        net_bs += leg.quantity * bs;
        net_mc += leg.quantity * mc;
        net_bin += leg.quantity * bin;
        net_delta += leg.quantity * delta;
        net_vega += leg.quantity * vega;
        rows.push_back({{"option_type", leg.option_type}, {"strike", leg.strike}, {"quantity", leg.quantity},
                        {"bs_price", bs}, {"mc_price", mc}, {"binomial_price", bin}, {"delta_bs", delta},
                        {"vega_bs", vega}, {"vol", v}, {"maturity", t},
                        {"forward", leg.forward > 0.0 ? leg.forward : 0.0}});
    }
    json summary = {{"net_bs_price", net_bs},     {"net_mc_price", net_mc},
                    {"net_binomial_price", net_bin}, {"net_delta", net_delta},
                    {"net_vega", net_vega},        {"strategy_hint", strategy_hint(legs, maturity)},
                    {"n_legs", static_cast<int>(legs.size())}};
    return ok_response(std::move(summary), {{"legs", std::move(rows)}}, t0_ms);
}

std::string run_greek_surface_json(const std::string& src, double t0_ms) {
    const auto req = parse_request(src, {"strike", "rate", "vol", "spot_min", "spot_max"},
                                   "missing_required_greek_surface_fields");
    if (!req) return req.error();
    const json& j = *req;
    const double strike = jv(j, "strike", 0.0), rate = jv(j, "rate", 0.0), vol = jv(j, "vol", 0.2);
    const auto greek = jv<std::string>(j, "greek", "delta");
    const int ns = std::max(2, std::min(jv(j, "n_spots", 21), 50));
    const int nt = std::max(2, std::min(jv(j, "n_mats", 11), 30));
    const double s_min = std::max(1e-6, jv(j, "spot_min", 0.0));
    const double s_max = std::max(s_min + 1e-6, jv(j, "spot_max", 0.0));
    const double m_min = std::max(1e-6, jv(j, "mat_min", 0.1));
    const double m_max = std::max(m_min + 1e-6, jv(j, "mat_max", 3.0));
    std::vector<double> spots(ns), mats(nt);
    for (int i = 0; i < ns; ++i) spots[i] = s_min + (s_max - s_min) * i / (ns - 1);
    for (int i = 0; i < nt; ++i) mats[i] = m_min + (m_max - m_min) * i / (nt - 1);
    const int code = greek == "gamma" ? 1 : greek == "vega" ? 2 : greek == "theta" ? 3 : greek == "rho" ? 4 : 0;
    std::vector<double> grid(ns * nt, 0.0);
    greek_surface_grid(strike, rate, vol, jv(j, "dividend_yield", 0.0), spots, mats, code, grid);
    json summary = {{"greek", greek},
                    {"n_spots", ns},
                    {"n_mats", nt},
                    {"grid_min", *std::ranges::min_element(grid)},
                    {"grid_max", *std::ranges::max_element(grid)},
                    {"strike", strike},
                    {"vol", vol},
                    {"rate", rate}};
    return ok_response(std::move(summary), {{"spots", spots}, {"maturities", mats}, {"grid", grid}}, t0_ms);
}

// The lattices alone, on a ladder of step counts: binomial (CRR) and trinomial prices per count
// against the Black-Scholes price (European even when is_american).
std::string run_lattice_json(const std::string& src, double t0_ms) {
    const auto req = parse_request(src, {"spot", "strike", "rate", "vol", "maturity", "steps"},
                                   "missing_required_lattice_fields");
    if (!req) return req.error();
    const auto& j = *req;
    if (!j["steps"].is_array() || j["steps"].empty()) return err_response("steps: a list of step counts");
    const double spot = jv(j, "spot", 0.0), strike = jv(j, "strike", 0.0);
    const double rate = jv(j, "rate", 0.0), vol = jv(j, "vol", 0.0);
    const double maturity = jv(j, "maturity", 0.0);
    const double q = jv(j, "dividend_yield", 0.0);
    const bool is_call = jv<std::string>(j, "option_type", "call") != "put";
    const bool american = jv(j, "is_american", false);
    const auto steps = j["steps"].get<std::vector<int>>();
    std::vector<double> binomial(steps.size()), trinomial(steps.size());
    for (std::size_t i = 0; i < steps.size(); ++i) {
        const int n = std::max(1, steps[i]);
        binomial[i] = binomial_crr_price(spot, strike, rate, q, vol, maturity, n, is_call, american);
        trinomial[i] = trinomial_price(spot, strike, rate, q, vol, maturity, n, is_call, american);
    }
    const double forward = spot_forward(spot, rate, q, maturity);
    json summary = {{"bs", black_price(is_call, forward, strike, maturity, rate, vol)},
                    {"option_type", is_call ? "call" : "put"},
                    {"is_american", american}};
    json details = {{"steps", steps}, {"binomial", binomial}, {"trinomial", trinomial}};
    return ok_response(std::move(summary), std::move(details), t0_ms);
}

// Black on each option's forward, discounted at `rate`: the trading engine's pricing (quant::black_*).
// With "vols": each price, and with "greeks": true its Greeks (to "spots", the forward when
// absent); with "prices": each implied vol (0: none).
std::string run_black_batch_json(const std::string& src, double t0_ms) {
    const auto req = parse_request(src, {"rate"}, "missing_required_black_batch_fields");
    if (!req) return req.error();
    const auto& j = *req;
    for (const std::string arr : {"is_call", "forwards", "strikes", "maturities"}) {
        if (!j.contains(arr) || !j[arr].is_array()) return err_response("missing_array_" + arr);
    }
    const bool pricing = j.contains("vols");
    if (pricing == j.contains("prices")) return err_response("give_vols_or_prices");
    const auto is_call = j["is_call"].get<std::vector<bool>>();
    const auto forwards = j["forwards"].get<std::vector<double>>();
    const auto strikes = j["strikes"].get<std::vector<double>>();
    const auto maturities = j["maturities"].get<std::vector<double>>();
    const auto values = j[pricing ? "vols" : "prices"].get<std::vector<double>>();
    const auto spots = j.contains("spots") ? j["spots"].get<std::vector<double>>() : forwards;
    const std::size_t n = is_call.size();
    if (forwards.size() != n || strikes.size() != n || maturities.size() != n || values.size() != n ||
        spots.size() != n)
        return err_response("array_length_mismatch");
    const double rate = jv(j, "rate", 0.0);
    if (pricing) {
        std::vector<double> price(n);
        for (std::size_t i = 0; i < n; ++i)
            price[i] = quant::black_price(is_call[i], forwards[i], strikes[i], maturities[i], rate, values[i]);
        if (!jv(j, "greeks", false))
            return ok_response({{"n_points", (int)n}, {"mode", "price"}}, {{"prices", price}}, t0_ms);
        std::vector<double> delta(n), gamma(n), theta(n), vega(n);
        for (std::size_t i = 0; i < n; ++i) {
            const auto g =
                quant::black_greeks(is_call[i], spots[i], forwards[i], strikes[i], maturities[i], rate, values[i]);
            delta[i] = g.delta;
            gamma[i] = g.gamma;
            theta[i] = g.theta;
            vega[i] = g.vega;
        }
        return ok_response({{"n_points", (int)n}, {"mode", "price"}},
                           {{"prices", price}, {"delta", delta}, {"gamma", gamma}, {"theta", theta}, {"vega", vega}},
                           t0_ms);
    }
    std::vector<double> ivs(n);
    for (std::size_t i = 0; i < n; ++i)
        ivs[i] = quant::black_implied_vol(values[i], forwards[i], strikes[i], maturities[i], rate, is_call[i]);
    return ok_response({{"n_points", (int)n}, {"mode", "implied_vol"}}, {{"ivs", ivs}}, t0_ms);
}

// A synthetic day's quotes (app/server optra.data.synthetic_days): each contract every minute at its
// Black price (on the forward, the spot grown at `rate`, discounted at it; at expiry its payoff) at the
// smile's vol, bid and ask a spread around it on the tick. In: "times" and "expiries" in seconds since
// the epoch, "spots" a minute, "strikes" and "is_call" a contract, the model's rate, implied_vol, skew,
// smile, tick, spread. Out: "bids" and "asks", minute by minute, a contract each.
std::string run_synthetic_quotes_json(const std::string& src, double t0_ms) {
    const auto req = parse_request(src, {"rate", "implied_vol", "tick", "spread"},
                                   "missing_required_synthetic_quotes_fields");
    if (!req) return req.error();
    const auto& j = *req;
    for (const std::string arr : {"times", "spots", "expiries", "strikes", "is_call"}) {
        if (!j.contains(arr) || !j[arr].is_array()) return err_response("missing_array_" + arr);
    }
    const auto times = j["times"].get<std::vector<std::int64_t>>();
    const auto spots = j["spots"].get<std::vector<double>>();
    const auto expiries = j["expiries"].get<std::vector<std::int64_t>>();
    const auto strikes = j["strikes"].get<std::vector<double>>();
    const auto is_call = j["is_call"].get<std::vector<bool>>();
    if (spots.size() != times.size() || strikes.size() != expiries.size() || is_call.size() != expiries.size())
        return err_response("array_length_mismatch");
    const double rate = jv(j, "rate", 0.0), iv = jv(j, "implied_vol", 0.0), skew = jv(j, "skew", 0.0),
                 smile = jv(j, "smile", 0.0), tick = jv(j, "tick", 0.0), spread = jv(j, "spread", 0.0);
    if (iv <= 0.0) return err_response("implied_vol_must_be_positive");
    constexpr double kYearSeconds = 365.0 * 24 * 3600; // model::kSecondsAYear
    constexpr double kMinT = 1e-6, kMinIv = 0.01, kMaxIv = 3.0;
    // Python's round(v, places), which the generator's quotes were made with: the float noise of a
    // division off before flooring onto the tick.
    const auto round_to = [](double v, double places) { return std::nearbyint(v * places) / places; };
    const std::size_t n = expiries.size();
    std::vector<double> bids(times.size() * n), asks(times.size() * n);
    for (std::size_t m = 0; m < times.size(); ++m) {
        for (std::size_t c = 0; c < n; ++c) {
            const std::int64_t secs = expiries[c] - times[m];
            const double t = secs <= 0 ? 0.0 : std::max(kMinT, static_cast<double>(secs) / kYearSeconds);
            const double forward = spots[m] * std::exp(rate * t);
            double price = 0.0;
            if (t <= 0.0) {
                price = std::max(is_call[c] ? forward - strikes[c] : strikes[c] - forward, 0.0);
            } else {
                double vol = iv;
                if (skew != 0.0 || smile != 0.0) {
                    const double z = std::log(strikes[c] / forward) / (iv * std::sqrt(t));
                    vol = std::clamp(iv + skew * z + smile * z * z, kMinIv, kMaxIv);
                }
                price = quant::black_price(is_call[c], forward, strikes[c], t, rate, vol);
            }
            const double half = std::max(tick, spread * price) / 2.0;
            double bid = 0.0, ask = 0.0;
            if (tick <= 0.0) {
                bid = std::max(price - half, 0.0);
                ask = price + half;
            } else {
                bid = round_to(std::max(std::floor(round_to((price - half) / tick, 1e9)) * tick, 0.0), 1e10);
                ask = round_to(std::max(std::ceil(round_to((price + half) / tick, 1e9)) * tick, tick), 1e10);
            }
            bids[(m * n) + c] = bid;
            asks[(m * n) + c] = ask;
        }
    }
    return ok_response({{"minutes", (int)times.size()}, {"contracts", (int)n}}, {{"bids", bids}, {"asks", asks}},
                       t0_ms);
}

}  // namespace quant::analytics
