#include "engine.h"
#include "quant/kernel.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace quant::analytics {

namespace {

// Threshold constants
constexpr double T_MEAN_ERR_SCALE    = 0.05;
constexpr double T_VAR_ERR_SCALE     = 0.10;
constexpr double T_DENSITY_INTEGRAL  = 0.25;
constexpr double T_DENS_CV           = 2.0;
constexpr double T_DENS_TAIL         = 100.0;
constexpr double T_ITO_ERR_SCALE     = 0.15;
constexpr double T_ITO_RESIDUAL      = 0.05;
constexpr double T_NORM_STD          = 0.25;
constexpr double T_TRI_VS_BS         = 1e-3;
constexpr double T_LSM_VS_BIN        = 0.02;
constexpr double T_PDE_VS_BIN        = 5e-3;

ValidationRow make_row(
    const std::string& cap, const std::string& metric,
    double value, double threshold, bool passed,
    const std::string& interp, const std::string& action
) {
    ValidationRow r;
    r.capability     = cap;
    r.metric         = metric;
    r.value          = value;
    r.threshold      = threshold;
    r.excess         = value - threshold;
    r.status         = passed ? "pass" : "fail";
    r.interpretation = interp;
    r.action         = action;
    return r;
}

}  // namespace

ValidationResult run_validation(const ValidationParams& p) {
    std::vector<ValidationRow> rows;

    if (p.pick_stats) {
        double mgf = 0.0, mean = 0.0, var = 0.0;
        stats_normal(p.mu, p.vol, p.stats_theta, p.stats_n, &mgf, &mean, &var);

        const double mean_err = std::abs(mean - p.mu);
        const double mean_tol = std::max(1e-6, T_MEAN_ERR_SCALE * std::max(std::abs(p.mu), 1.0));
        rows.push_back(make_row("stats", "mean_error", mean_err, mean_tol, mean_err <= mean_tol,
            "sample mean check", "increase sample_size or lower sigma"));

        const double var_err = std::abs(var - p.vol * p.vol);
        const double var_tol = std::max(1e-6, T_VAR_ERR_SCALE * std::max(p.vol * p.vol, 1.0));
        rows.push_back(make_row("stats", "variance_error", var_err, var_tol, var_err <= var_tol,
            "sample variance check", "increase sample_size or verify sigma input"));
    }

    if (p.pick_ito) {
        const int fcode = (p.ito_function_type == "w2_minus_t") ? 1
                        : (p.ito_function_type == "w3")         ? 2 : 0;
        const ItoCheck c = ito_check(fcode, p.ito_theta, p.ito_t, p.ito_n);
        const double diff = std::abs(c.value - c.target);
        const double tol  = std::max(1e-3, T_ITO_ERR_SCALE * std::max(std::abs(c.target), 1.0));
        rows.push_back(make_row("ito", "expectation_gap", diff, tol, diff <= tol,
            "martingale expectation", "lower theta or t: with a large theta^2 t the mean rests on rare paths"));
        rows.push_back(make_row("ito", "formula_residual", c.residual, T_ITO_RESIDUAL, c.residual <= T_ITO_RESIDUAL,
            "Ito's formula on discrete paths", "increase n_steps: the gap shrinks like sqrt(dt)"));
    }

    if (p.pick_simulation) {
        // Simulation finite check
        const int sim_n = std::max(p.sim_steps, 1);
        std::vector<double> path(sim_n + 1, 0.0);
        const int len = simulation_path(0, sim_n, 0.01, p.vol, 1.2, 0.03, 0.0, path);
        const double finite = (len > 0 && std::isfinite(path[len - 1])) ? 1.0 : 0.0;
        {
            ValidationRow r;
            r.capability = "simulation"; r.metric = "terminal_finite";
            r.value = finite; r.threshold = 1.0;
            r.excess = finite - 1.0;
            r.status = finite >= 1.0 ? "pass" : "fail";
            r.interpretation = "terminal path finite check";
            r.action = "reduce dt or sigma; verify model parameters";
            rows.push_back(r);
        }

        // Hedging normalized std check: one call sold, discrete delta hedging
        const HedgeLeg call{true, p.strike, -1.0, p.vol, p.maturity, 0.0};
        const auto runs = hedge_position(p.spot, p.rate, 0.0, p.vol, std::span(&call, 1),
                                         p.n_rebalances, p.hedge_paths, 0.0, 0.0, 1.0);
        const double norm_std = hedge_stats(runs.front()).std / std::max(p.spot, 1e-12);
        rows.push_back(make_row("hedging", "normalized_std", norm_std, T_NORM_STD, norm_std <= T_NORM_STD,
            "hedging distribution spread", "increase rebalances or reduce vol/maturity"));
    }

    if (p.pick_lattice) {
        // European: Boyle trinomial (400 steps) against Black-Scholes, call.
        const double bs  = bs_closed_form_price(p.spot, p.strike, p.rate, p.vol, p.maturity, true);
        const double tri = trinomial_price(p.spot, p.strike, p.rate, 0.0, p.vol, p.maturity, 400, true, false);
        const double tri_rel = std::abs(tri - bs) / std::max(std::abs(bs), 1e-12);
        rows.push_back(make_row("lattice", "trinomial_vs_bs_rel", tri_rel, T_TRI_VS_BS, tri_rel <= T_TRI_VS_BS,
            "trinomial lattice vs closed form", "increase lattice steps or check vol / maturity inputs"));

        // American put: LSM and CN-PSOR against a 1000-step binomial tree.
        const double bin = binomial_crr_price(p.spot, p.strike, p.rate, 0.0, p.vol, p.maturity, 1000, false, true);
        const LsmResult lsm = lsm_american_price(p.spot, p.strike, p.rate, 0.0, p.vol, p.maturity, 50000, 50, false);
        const double lsm_rel = std::abs(lsm.price - bin) / std::max(std::abs(bin), 1e-12);
        rows.push_back(make_row("lattice", "lsm_vs_binomial_american_rel", lsm_rel, T_LSM_VS_BIN, lsm_rel <= T_LSM_VS_BIN,
            "Longstaff-Schwartz vs binomial American put", "increase LSM paths / exercise dates"));

        PdeSpec spec;
        spec.spot = p.spot; spec.strike = p.strike; spec.rate = p.rate; spec.vol = p.vol;
        spec.maturity = p.maturity; spec.s_steps = 300; spec.t_steps = 300;
        spec.is_call = false; spec.american = true;
        const double pde = pde_solve(spec).price;
        const double pde_rel = std::abs(pde - bin) / std::max(std::abs(bin), 1e-12);
        rows.push_back(make_row("lattice", "pde_vs_binomial_american_rel", pde_rel, T_PDE_VS_BIN, pde_rel <= T_PDE_VS_BIN,
            "PSOR finite difference vs binomial American put", "increase PDE grid or check boundary setup"));
    }

    // Summarize
    ValidationResult r;
    r.mode         = p.block_mode ? "blocking" : "advisory";
    r.checks_total = static_cast<int>(rows.size());
    r.rows         = rows;

    int failed = 0;
    std::string fail_parts;
    double max_viol = 0.0;
    for (const auto& row : rows) {
        if (row.status == "fail") {
            ++failed;
            if (!fail_parts.empty()) fail_parts += "; ";
            fail_parts += row.capability + "." + row.metric;
        }
        const double viol = std::max(0.0, row.value - row.threshold);
        if (viol > max_viol) {
            max_viol = viol;
            r.max_excess_item = row.capability + "." + row.metric;
        }
    }
    r.checks_failed  = failed;
    r.fail_rate      = rows.empty() ? 0.0 : static_cast<double>(failed) / rows.size();
    r.max_excess     = max_viol;
    r.blocking_reason = fail_parts.empty() ? "none" : fail_parts;

    const bool passed = (failed == 0);
    if (passed)
        r.gate_decision = "go";
    else
        r.gate_decision = p.block_mode ? "blocked" : "warning";

    return r;
}

}  // namespace quant::analytics
