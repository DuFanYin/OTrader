// Validation gate: the theory checks (sample moments, Ito, simulation and hedging, the lattices, the
// smile fit), each against its threshold, with the fix for each that fails.
#include "json_io.h"
#include "quant/kernel.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace quant::analytics {

namespace {

// Threshold constants
constexpr double T_MEAN_ERR_SCALE    = 0.05;
constexpr double T_VAR_ERR_SCALE     = 0.10;
constexpr double T_ITO_ERR_SCALE     = 0.15;
constexpr double T_ITO_RESIDUAL      = 0.05;
constexpr double T_NORM_STD          = 0.25;
constexpr double T_TRI_VS_BS         = 1e-3;
constexpr double T_LSM_VS_BIN        = 0.02;
constexpr double T_PDE_VS_BIN        = 5e-3;
constexpr double T_SMILE_ERROR       = 1.0; // the fit's rms residual, in half spreads

// A check against its threshold; `param` (when not empty) is the request's field that cures a
// failure, at `proposed` instead of `current`.
json make_row(const std::string& cap, const std::string& metric, double value, double threshold, bool passed,
              const std::string& interp, const std::string& action, const std::string& param = "",
              double current = 0.0, double proposed = 0.0) {
    const bool fix = !param.empty();
    return {{"capability", cap}, {"metric", metric}, {"value", value}, {"threshold", threshold},
            {"excess", value - threshold}, {"status", passed ? "pass" : "fail"}, {"interpretation", interp},
            {"action", action}, {"param", fix ? json(param) : json(nullptr)},
            {"current_value", fix ? number(current) : json(nullptr)},
            {"proposed_value", fix ? number(proposed) : json(nullptr)}};
}

// The request's inputs the checks read.
struct Inputs {
    double spot, strike, rate, vol, maturity, mu;
    bool pick_stats, pick_ito, pick_simulation, pick_lattice, pick_smile, block_mode;
    double stats_theta;
    int stats_n;
    double ito_theta, ito_t;
    int ito_n;
    std::string ito_function_type;
    int sim_steps, n_rebalances, hedge_paths;
};

Inputs read_inputs(const json& j) {
    return {jv(j, "spot", 0.0), jv(j, "strike", 0.0), jv(j, "rate", 0.0), jv(j, "vol", 0.0),
            jv(j, "maturity", 0.0), jv(j, "mu", 0.0), jv(j, "pick_stats", false), jv(j, "pick_ito", false),
            jv(j, "pick_simulation", false), jv(j, "pick_lattice", false), jv(j, "pick_smile", false),
            jv(j, "compute_block_on_validation", false), jv(j, "stats_theta", 1.0), jv(j, "stats_n", 10000),
            jv(j, "ito_theta", 0.7), jv(j, "ito_t", 1.0), jv(j, "ito_n", 2000),
            jv<std::string>(j, "ito_function_type", "exp_martingale"), jv(j, "sim_steps", 100),
            jv(j, "n_rebalances", 52), jv(j, "hedge_paths", 500)};
}

}  // namespace

std::string run_validation_json(const std::string& src, double t0_ms) {
    const auto req = parse_request(src, {"spot", "strike", "rate", "vol", "maturity"},
                                   "missing_required_validation_fields");
    if (!req) return req.error();
    const Inputs p = read_inputs(*req);
    json rows = json::array();

    if (p.pick_stats) {
        double mgf = 0.0, mean = 0.0, var = 0.0;
        stats_normal(p.mu, p.vol, p.stats_theta, p.stats_n, &mgf, &mean, &var);

        const double mean_err = std::abs(mean - p.mu);
        const double mean_tol = std::max(1e-6, T_MEAN_ERR_SCALE * std::max(std::abs(p.mu), 1.0));
        const double more = 4.0 * std::max(p.stats_n, 10000);
        rows.push_back(make_row("stats", "mean_error", mean_err, mean_tol, mean_err <= mean_tol,
            "sample mean check", "a larger sample: the error falls like 1/sqrt(n)",
            "stats_n", p.stats_n, more));

        const double var_err = std::abs(var - p.vol * p.vol);
        const double var_tol = std::max(1e-6, T_VAR_ERR_SCALE * std::max(p.vol * p.vol, 1.0));
        rows.push_back(make_row("stats", "variance_error", var_err, var_tol, var_err <= var_tol,
            "sample variance check", "a larger sample: the error falls like 1/sqrt(n)",
            "stats_n", p.stats_n, more));
    }

    if (p.pick_ito) {
        const int fcode = (p.ito_function_type == "w2_minus_t") ? 1
                        : (p.ito_function_type == "w3")         ? 2 : 0;
        const ItoCheck c = ito_check(fcode, p.ito_theta, p.ito_t, p.ito_n);
        const double diff = std::abs(c.value - c.target);
        const double tol  = std::max(1e-3, T_ITO_ERR_SCALE * std::max(std::abs(c.target), 1.0));
        rows.push_back(make_row("ito", "expectation_gap", diff, tol, diff <= tol,
            "martingale expectation",
            "lower theta or t: with a large theta^2 t the mean rests on rare paths",
            "ito_theta", p.ito_theta, p.ito_theta / 2.0));
        rows.push_back(make_row("ito", "formula_residual", c.residual, T_ITO_RESIDUAL, c.residual <= T_ITO_RESIDUAL,
            "Ito's formula on discrete paths", "more steps: the gap shrinks like sqrt(dt)",
            "ito_n", p.ito_n, 2.0 * std::max(p.ito_n, 2000)));
    }

    if (p.pick_simulation) {
        // Simulation finite check
        const int sim_n = std::max(p.sim_steps, 1);
        std::vector<double> path(sim_n + 1, 0.0);
        const int len = simulation_path(0, sim_n, 0.01, p.vol, 1.2, 0.03, 0.0, path);
        const double finite = (len > 0 && std::isfinite(path[len - 1])) ? 1.0 : 0.0;
        rows.push_back(make_row("simulation", "terminal_finite", finite, 1.0, finite >= 1.0,
            "terminal path finite check", "lower vol: the path's end is not a finite number"));

        // Hedging normalized std check: one call sold, discrete delta hedging
        const HedgeLeg call{true, p.strike, -1.0, p.vol, p.maturity, 0.0};
        const auto runs = hedge_position(p.spot, p.rate, 0.0, p.vol, std::span(&call, 1),
                                         p.n_rebalances, p.hedge_paths, 0.0, 0.0, 1.0);
        const double norm_std = hedge_stats(runs.front()).std / std::max(p.spot, 1e-12);
        rows.push_back(make_row("hedging", "normalized_std", norm_std, T_NORM_STD, norm_std <= T_NORM_STD,
            "hedging distribution spread",
            "rebalance more often: the hedge error falls like 1/sqrt(rebalances)",
            "n_rebalances", p.n_rebalances, 2.0 * std::max(p.n_rebalances, 52)));
    }

    if (p.pick_lattice) {
        // European: Boyle trinomial (400 steps) against Black-Scholes, call.
        const double fwd = spot_forward(p.spot, p.rate, 0.0, p.maturity);
        const double bs  = black_price(true, fwd, p.strike, p.maturity, p.rate, p.vol);
        const double tri = trinomial_price(p.spot, p.strike, p.rate, 0.0, p.vol, p.maturity, 400, true, false);
        const double tri_rel = std::abs(tri - bs) / std::max(std::abs(bs), 1e-12);
        rows.push_back(make_row("lattice", "trinomial_vs_bs_rel", tri_rel, T_TRI_VS_BS, tri_rel <= T_TRI_VS_BS,
            "trinomial lattice vs closed form",
            "check the vol and maturity: the tree has 400 steps"));

        // American put: LSM and CN-PSOR against a 1000-step binomial tree.
        const double bin = binomial_crr_price(p.spot, p.strike, p.rate, 0.0, p.vol, p.maturity, 1000, false, true);
        const LsmResult lsm = lsm_american_price(p.spot, p.strike, p.rate, 0.0, p.vol, p.maturity, 50000, 50, false);
        const double lsm_rel = std::abs(lsm.price - bin) / std::max(std::abs(bin), 1e-12);
        rows.push_back(make_row("lattice", "lsm_vs_binomial_american_rel", lsm_rel, T_LSM_VS_BIN, lsm_rel <= T_LSM_VS_BIN,
            "Longstaff-Schwartz vs binomial American put",
            "check the inputs: LSM runs 50,000 paths of 50 dates"));

        PdeSpec spec;
        spec.spot = p.spot; spec.strike = p.strike; spec.rate = p.rate; spec.vol = p.vol;
        spec.maturity = p.maturity; spec.s_steps = 300; spec.t_steps = 300;
        spec.is_call = false; spec.american = true;
        const double pde = pde_solve(spec).price;
        const double pde_rel = std::abs(pde - bin) / std::max(std::abs(bin), 1e-12);
        rows.push_back(make_row("lattice", "pde_vs_binomial_american_rel", pde_rel, T_PDE_VS_BIN, pde_rel <= T_PDE_VS_BIN,
            "PSOR finite difference vs binomial American put",
            "check the inputs: the grid is 300 x 300"));
    }

    if (p.pick_smile) {
        // A smile like an index's (vol at the money, the put side steeper), its total variances
        // off by up to half their half spread (2% of each): fitted, it should be within the spread
        // and free of butterfly arbitrage; Axel Vogt's smile should not be.
        const double sd = p.vol * std::sqrt(p.maturity);
        const Svi made{.a = 0.6 * sd * sd, .b = 0.4 * sd, .rho = -0.5, .m = 0.0, .sigma = sd};
        std::vector<double> k, w, half;
        for (int i = 0; i <= 40; ++i) {
            const double x = sd * (-3.0 + (6.0 * i / 40.0));
            const double v = made.total_variance(x);
            k.push_back(x);
            half.push_back(0.02 * v);
            w.push_back(v + (0.5 * half.back() * std::sin(1.7 * i)));
        }
        const SviFit fit = svi_fit(k, w, half);
        rows.push_back(make_row("smile", "fit_error_half_spreads", fit.error, T_SMILE_ERROR,
            fit.points > 0 && fit.error <= T_SMILE_ERROR, "SVI fit to a smile noisy within its spread",
            "check the vol and maturity: the smile is 41 strikes over 3 deviations either side"));
        const double free = fit.svi.butterfly_free(k.front(), k.back()) ? 1.0 : 0.0;
        rows.push_back(make_row("smile", "butterfly_free", free, 1.0, free >= 1.0,
            "the fitted smile's density not negative", "check the vol and maturity"));
        const Svi vogt{.a = -0.0410, .b = 0.1331, .rho = 0.3060, .m = 0.3586, .sigma = 0.4153};
        const double found = vogt.butterfly_free(-1.5, 1.5) ? 0.0 : 1.0;
        rows.push_back(make_row("smile", "arbitrage_found", found, 1.0, found >= 1.0,
            "the butterfly check finds Axel Vogt's arbitrage", "the check itself is broken"));
    }

    int failed = 0;
    std::string fail_parts, max_excess_item;
    double max_viol = 0.0;
    for (const auto& row : rows) {
        const std::string item = row["capability"].get<std::string>() + "." + row["metric"].get<std::string>();
        if (row["status"] == "fail") {
            ++failed;
            if (!fail_parts.empty()) fail_parts += "; ";
            fail_parts += item;
        }
        const double viol = std::max(0.0, row["value"].get<double>() - row["threshold"].get<double>());
        if (viol > max_viol) {
            max_viol = viol;
            max_excess_item = item;
        }
    }
    const int total = static_cast<int>(rows.size());
    json summary = {{"validation_mode", p.block_mode ? "blocking" : "advisory"},
                    {"gate_decision", failed == 0 ? "go" : p.block_mode ? "blocked" : "warning"},
                    {"checks_total", total},
                    {"checks_failed", failed},
                    {"fail_rate", total == 0 ? 0.0 : static_cast<double>(failed) / total},
                    {"max_excess", max_viol},
                    {"max_excess_item", max_excess_item}};
    return ok_response(std::move(summary), {{"rows", std::move(rows)}}, t0_ms);
}

}  // namespace quant::analytics
