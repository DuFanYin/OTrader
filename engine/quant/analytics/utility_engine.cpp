#include "engine.h"
#include "quant/kernel.h"

#include <algorithm>
#include <cmath>
#include <ranges>

namespace quant::analytics {

SimulationResult run_simulation(const SimulationParams& p) {
    const int n = std::max(1, p.n_steps);
    std::vector<double> buf(static_cast<size_t>(n + 1));
    const int model_code = (p.model == "vasicek") ? 1 : 0;
    const int length = simulation_path(
        model_code, n, p.dt, p.sigma, p.kappa, p.theta, p.x0, buf);
    buf.resize(static_cast<size_t>(std::max(0, length)));
    return SimulationResult{p.model, n, std::move(buf)};
}

StatsResult run_stats(const StatsParams& p) {
    double mgf = 0.0, mean = 0.0, var = 0.0;
    stats_normal(p.mu, p.sigma, p.theta, p.sample_size, &mgf, &mean, &var);
    const double s = clamp_positive(p.sigma);
    return StatsResult{mgf, mean, var, stats_mgf(p.mu, p.sigma, p.theta), p.mu, s * s};
}

ItoResult run_ito(const ItoParams& p) {
    const int code = (p.function_type == "w2_minus_t") ? 1
                   : (p.function_type == "w3")         ? 2 : 0;
    const ItoCheck c = ito_check(code, p.theta, p.t, p.n_steps);
    return ItoResult{p.function_type, c.value, c.target, c.std_error, c.residual, c.paths};
}

MeasureDensityResult run_measure_density(const MeasureDensityParams& p) {
    const int n = std::max(20, p.n_steps);
    std::vector<double> buf(static_cast<size_t>(n + 1));
    const int length = measure_density_path(p.mu, p.r, p.sigma, p.t, n, buf);
    buf.resize(static_cast<size_t>(std::max(0, length)));
    return MeasureDensityResult{std::move(buf)};
}

MeasureCompareResult run_measure_compare(const MeasureCompareParams& p) {
    const int pl = std::max(1, std::min(p.preview_len, p.n_steps));
    std::vector<double> stats_buf(10, 0.0);
    std::vector<double> preview_buf(static_cast<size_t>(2 * pl), 0.0);
    measure_compare(
        p.mu, p.r, p.sigma, p.t, p.n_steps, p.n_paths, p.x0,
        stats_buf, preview_buf, pl);
    MeasureCompareResult r;
    r.p_mean = stats_buf[0]; r.p_var = stats_buf[1];
    r.p_q05  = stats_buf[2]; r.p_q50 = stats_buf[3]; r.p_q95 = stats_buf[4];
    r.q_mean = stats_buf[5]; r.q_var = stats_buf[6];
    r.q_q05  = stats_buf[7]; r.q_q50 = stats_buf[8]; r.q_q95 = stats_buf[9];
    r.preview_p.assign(preview_buf.begin(), preview_buf.begin() + pl);
    r.preview_q.assign(preview_buf.begin() + pl, preview_buf.begin() + 2 * pl);
    return r;
}

PdeResult run_pde(const PdeParams& p) {
    PdeSpec spec;
    spec.spot = p.spot; spec.strike = p.strike; spec.rate = p.rate; spec.dividend_yield = p.dividend_yield;
    spec.vol = p.vol; spec.maturity = p.maturity; spec.s_steps = p.s_steps; spec.t_steps = p.t_steps;
    spec.method   = p.method == "implicit" ? 1 : (p.method == "explicit" ? 2 : 0);
    spec.is_call  = p.option_type != "put";
    spec.american = p.is_american;
    const PdeOutcome o = pde_solve(spec);
    PdeResult r;
    r.price = o.price;
    r.method = spec.method == 1 ? "implicit" : (spec.method == 2 ? "explicit" : "crank_nicolson");
    r.s_steps = p.s_steps;
    r.t_steps = p.t_steps;
    r.t_steps_used = o.t_steps_used;
    r.refined = o.refined;
    r.psor_iterations = o.psor_iterations;
    r.option_type = spec.is_call ? "call" : "put";
    r.is_american = p.is_american;
    return r;
}

VolSurfaceResult run_vol_surface(const VolSurfaceParams& p) {
    if (p.strikes.empty() || p.spot <= 0.0 || p.target_strike <= 0.0 || p.target_expiry <= 0.0)
        return {0.0, "error"};
    const double iv = vol_surface_interp(
        p.strikes, p.expiries, p.ivs, p.spot, p.target_strike, p.target_expiry);
    if (iv < 0.0) return {0.0, "error"};

    // Determine if true bilinear interpolation was used or fell back to nearest
    std::vector<double> um = p.strikes, uv = p.expiries;
    std::ranges::sort(um);
    um.erase(std::ranges::unique(um).begin(), um.end());
    std::ranges::sort(uv);
    uv.erase(std::ranges::unique(uv).begin(), uv.end());
    const std::string method = (um.size() >= 2 && uv.size() >= 2) ? "cpp_bilinear" : "cpp_nearest";
    return {iv, method};
}

}  // namespace quant::analytics
