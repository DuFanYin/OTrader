// The other tasks on quant: simulated paths, sample moments, Ito's formula, the measures P and Q,
// the PDE, the vol surface, selection among an experiment's trials, and the optimizer's next points.
#include "json_io.h"
#include "quant/kernel.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <ranges>
#include <span>
#include <vector>

namespace quant {

int simulation_path(
    int model_code,
    int n_steps,
    double dt,
    double sigma,
    double kappa,
    double theta,
    double x0,
    std::span<double> out_values
) {
    std::mt19937_64 rng(7);
    std::normal_distribution<double> z(0.0, 1.0);
    const int n = std::max(n_steps, 1);
    const double dtt = clamp_positive(dt);
    ModelParams params{};
    params.sigma = sigma;
    params.kappa = kappa;
    params.theta = theta;
    auto model = build_model(
        model_code == 1 ? ModelType::Vasicek : ModelType::GBM,
        params
    );

    if (out_values.size() < static_cast<size_t>(n + 1)) return 0;
    double x = x0;
    out_values[0] = x;
    for (int i = 0; i < n; ++i) {
        x = model->step(x, i * dtt, dtt, z(rng));
        out_values[i + 1] = x;
    }
    return n + 1;
}

int measure_density_path(
    double mu,
    double r,
    double sigma,
    double t,
    int n_steps,
    std::span<double> out_values
) {
    std::mt19937_64 rng(23);
    std::normal_distribution<double> z(0.0, 1.0);
    const int n = std::max(n_steps, 20);
    const double s = clamp_positive(sigma);
    const double tt = clamp_positive(t);
    const double dt = tt / static_cast<double>(n);
    const double theta_mpr = (mu - r) / s;
    if (out_values.size() < static_cast<size_t>(n + 1)) return 0;

    double w = 0.0;
    out_values[0] = 1.0;
    for (int i = 0; i < n; ++i) {
        w += std::sqrt(dt) * z(rng);
        out_values[i + 1] = std::exp(-theta_mpr * w - 0.5 * theta_mpr * theta_mpr * ((i + 1) * dt));
    }
    return n + 1;
}

int measure_compare(
    double  mu,
    double  r,
    double  sigma,
    double  t,
    int     n_steps,
    int     n_paths,
    double  x0,
    std::span<double> out_stats_10,
    std::span<double> out_preview,
    int     preview_len
) {
    if (out_stats_10.size() < 10) return 0;
    const int    N       = std::max(n_steps, 2);
    const int    NP      = std::max(n_paths, 1);
    const double s       = clamp_positive(sigma, 1e-10);
    const double tt      = clamp_positive(t);
    const double dt      = tt / N;
    const int    PL      = std::min(preview_len, N);
    if (out_preview.size() < static_cast<size_t>(2 * PL)) return 0;

    std::mt19937_64 rng(42);
    std::normal_distribution<double> zdist(0.0, 1.0);
    ModelParams p_params{};
    p_params.mu = mu;
    p_params.sigma = s;
    ModelParams q_params{};
    q_params.mu = r;
    q_params.sigma = s;

    {
        auto p_model = build_model(ModelType::GBM, p_params);
        auto q_model = build_model(ModelType::GBM, q_params);
        double xp = x0, xq = x0;
        for (int i = 0; i < PL; ++i) {
            const double zi = zdist(rng);
            xp = p_model->step(xp, i * dt, dt, zi);
            xq = q_model->step(xq, i * dt, dt, zi);
            out_preview[i]      = xp;
            out_preview[PL + i] = xq;
        }
    }

    std::vector<double> fp(NP), fq(NP);

    // A random stream per chunk of paths, not per thread: the result is the same on any number of
    // threads.
    constexpr int kChunk = 64;
    const int chunks = (NP + kChunk - 1) / kChunk;
    #pragma omp parallel for schedule(dynamic)
    for (int c = 0; c < chunks; ++c) {
        std::mt19937_64 lrng(42 + static_cast<unsigned>(c));
        std::normal_distribution<double> lz(0.0, 1.0);
        auto p_model = build_model(ModelType::GBM, p_params);
        auto q_model = build_model(ModelType::GBM, q_params);
        for (int k = c * kChunk; k < std::min(NP, (c + 1) * kChunk); ++k) {
            double xp = x0, xq = x0;
            for (int i = 0; i < N; ++i) {
                const double zi = lz(lrng);
                xp = p_model->step(xp, i * dt, dt, zi);
                xq = q_model->step(xq, i * dt, dt, zi);
            }
            fp[k] = xp; fq[k] = xq;
        }
    }

    auto stats5 = [&](std::vector<double>& v, std::span<double> out) {
        std::ranges::sort(v);
        out[0] = sample_mean(v);
        out[1] = sample_variance(v);
        out[2] = interp_quantile_sorted(v, 0.05);
        out[3] = interp_quantile_sorted(v, 0.50);
        out[4] = interp_quantile_sorted(v, 0.95);
    };
    stats5(fp, out_stats_10.subspan<0, 5>());
    stats5(fq, out_stats_10.subspan<5, 5>());
    return PL;
}

double vol_surface_interp(
    std::span<const double> strikes,
    std::span<const double> expiries,
    std::span<const double> ivs,
    double spot, double target_strike, double target_expiry
) {
    const int n_points = static_cast<int>(strikes.size());
    if (expiries.size() != strikes.size() || ivs.size() != strikes.size() || n_points < 1 ||
        spot <= 0.0
            || target_strike <= 0.0 || target_expiry <= 0.0)
        return -1.0;

    const double tgt_m = std::log(target_strike / spot);
    const double tgt_v = std::sqrt(target_expiry);
    std::vector<double> ms(n_points), vs(n_points);
    for (int i = 0; i < n_points; ++i) {
        ms[i] = std::log(strikes[i] / spot);
        vs[i] = std::sqrt(clamp_positive(expiries[i]));
    }
    std::vector<double> um = ms, uv = vs;
    std::ranges::sort(um);
    um.erase(std::ranges::unique(um).begin(), um.end());
    std::ranges::sort(uv);
    uv.erase(std::ranges::unique(uv).begin(), uv.end());

    if (um.size() < 2 || uv.size() < 2) {
        int best = 0; double best_d2 = 1e30;
        for (int i = 0; i < n_points; ++i) {
            const double dm = ms[i] - tgt_m, dv = vs[i] - tgt_v;
            const double d2 = dm * dm + dv * dv;
            if (d2 < best_d2) { best_d2 = d2; best = i; }
        }
        return ivs[best];
    }

    auto bracket = [](const std::vector<double>& axis, double t) -> std::pair<int, int> {
        if (t <= axis.front()) return {0, 0};
        if (t >= axis.back())  return {(int)axis.size() - 1, (int)axis.size() - 1};
        for (int i = 0; i < (int)axis.size() - 1; ++i)
            if (axis[i] <= t && t <= axis[i + 1]) return {i, i + 1};
        return {(int)axis.size() - 1, (int)axis.size() - 1};
    };
    auto [mi0, mi1] = bracket(um, tgt_m);
    auto [vi0, vi1] = bracket(uv, tgt_v);
    const double m0 = um[mi0], m1 = um[mi1];
    const double v0 = uv[vi0], v1 = uv[vi1];
    const double wm = (m1 > m0) ? (tgt_m - m0) / (m1 - m0) : 0.0;
    const double wv = (v1 > v0) ? (tgt_v - v0) / (v1 - v0) : 0.0;

    auto corner_iv = [&](double cm, double cv) -> double {
        int best = 0; double best_d = 1e30;
        for (int i = 0; i < n_points; ++i) {
            const double dm = ms[i] - cm, dv = vs[i] - cv;
            const double d = dm * dm + dv * dv;
            if (d < best_d) { best_d = d; best = i; }
        }
        return ivs[best];
    };

    const double q00 = corner_iv(m0, v0), q10 = corner_iv(m1, v0);
    const double q01 = corner_iv(m0, v1), q11 = corner_iv(m1, v1);
    const double interp = q00*(1-wm)*(1-wv) + q10*wm*(1-wv) + q01*(1-wm)*wv + q11*wm*wv;
    return std::isfinite(interp) ? interp : -1.0;
}

}  // namespace quant

namespace quant::analytics {

namespace {

constexpr int kSimulationPreviewCap = 200;

int ito_code(const std::string& f) { return f == "w2_minus_t" ? 1 : f == "w3" ? 2 : 0; }

}  // namespace

std::string run_simulation_json(const std::string& src, double t0_ms) {
    const auto req = parse_request(src);
    if (!req) return req.error();
    const json& j = *req;
    const auto model = jv<std::string>(j, "model", "gbm");
    const int n = std::max(1, jv(j, "n_steps", 100));
    std::vector<double> values(static_cast<std::size_t>(n + 1));
    const int length = simulation_path(model == "vasicek" ? 1 : 0, n, jv(j, "dt", 0.01), jv(j, "sigma", 0.2),
                                       jv(j, "kappa", 1.2), jv(j, "theta", 0.03), jv(j, "x0", 1.0), values);
    values.resize(static_cast<std::size_t>(std::max(0, length)));
    const int size = static_cast<int>(values.size());
    const int stride = size > kSimulationPreviewCap ? size / kSimulationPreviewCap : 1;
    json preview = json::array();
    for (int i = 0; i < size; i += stride) preview.push_back(values[i]);
    json summary = {{"model", model},
                    {"n_steps", n},
                    {"x_final", values.empty() ? 0.0 : values.back()},
                    {"x_min", values.empty() ? 0.0 : *std::ranges::min_element(values)},
                    {"x_max", values.empty() ? 0.0 : *std::ranges::max_element(values)},
                    {"length", size}};
    return ok_response(std::move(summary), {{"values_preview", std::move(preview)}}, t0_ms);
}

// Sample moments of a normal, with the exact ones they estimate.
std::string run_stats_json(const std::string& src, double t0_ms) {
    const auto req = parse_request(src);
    if (!req) return req.error();
    const json& j = *req;
    const double mu = jv(j, "mu", 0.0), sigma = jv(j, "sigma", 1.0), theta = jv(j, "theta", 1.0);
    double mgf = 0.0, mean = 0.0, var = 0.0;
    stats_normal(mu, sigma, theta, jv(j, "sample_size", 10000), &mgf, &mean, &var);
    const double s = clamp_positive(sigma);
    json summary = {{"mgf", mgf},
                    {"mean", mean},
                    {"variance", var},
                    {"mgf_exact", stats_mgf(mu, sigma, theta)},
                    {"mean_exact", mu},
                    {"variance_exact", s * s}};
    return ok_response(std::move(summary), json::object(), t0_ms);
}

// Selection among an experiment's trials: `returns` their returns by period, row-major trials x periods (NaN where a
// trial has none), the request's "trials" and "periods" its shape, "tries" how many tries its family has made in all
// (kernel.h: deflated_sharpe, backtest_overfitting, walk_forward).
std::string run_selection_json(const std::string& src, std::span<const double> returns, double t0_ms) {
    const auto req = parse_request(src, {"trials", "periods"}, "trials and periods: the returns' shape");
    if (!req) return req.error();
    const json& body = *req;
    const int trials = jv(body, "trials", 0), periods = jv(body, "periods", 0);
    if (trials < 1 || periods < 1 || returns.size() != static_cast<std::size_t>(trials) * periods)
        return err_response("returns: trials x periods values, row by row");
    const int given_tries = jv(body, "tries", 0);
    const int tries = given_tries > 0 ? given_tries : trials;
    const int given_train = jv(body, "train", 0), given_test = jv(body, "test", 0);
    const int train = given_train > 0 ? given_train : std::max(2, periods / 3);
    const int test = given_test > 0 ? given_test : std::max(1, periods / 6);
    const DeflatedSharpe d = deflated_sharpe(returns, trials, periods, tries);
    const Overfitting o = backtest_overfitting(returns, trials, periods, jv(body, "blocks", 16),
                                               std::max(1, jv(body, "max_splits", 5000)),
                                               jv<std::uint64_t>(body, "seed", 0));
    const WalkForward wf = walk_forward(returns, trials, periods, train, test);
    json windows = json::array();
    for (const WalkForwardWindow& w : wf.windows) {
        windows.push_back({{"train_first", w.train_first}, {"test_first", w.test_first}, {"best", w.best},
                           {"test_net", w.test_net}, {"test_sharpe", w.test_sharpe}});
    }
    json summary = {
        {"deflated_sharpe", {{"best", d.best}, {"sharpe", d.sharpe}, {"expected_max_sharpe", d.expected_max_sharpe},
                             {"skew", d.skew}, {"kurtosis", d.kurtosis}, {"periods", d.periods},
                             {"tries", tries}, {"dsr", d.valid ? json(d.dsr) : json(nullptr)}}},
        {"pbo", {{"pbo", o.valid ? json(o.pbo) : json(nullptr)}, {"splits", o.splits}, {"blocks", o.blocks},
                 {"logit_median", o.logit_median}}},
        {"walk_forward", {{"train", train}, {"test", test}, {"net", wf.net}, {"sharpe", wf.sharpe},
                          {"positive_windows", wf.positive}, {"windows", std::move(windows)}}},
    };
    return ok_response(std::move(summary), json::object(), t0_ms);
}

// {"lower": [...], "upper": [...], "integer": [bool...], "tried": [[...]...], "values": [number|null...],
// "batch", "seed"}: bayes_next's points.
std::string run_optimize_json(const std::string& src, double t0_ms) {
    const auto req = parse_request(src);
    if (!req) return req.error();
    const json& body = *req;
    const auto lower = body.value("lower", std::vector<double>{});
    const auto upper = body.value("upper", std::vector<double>{});
    const auto whole = body.value("integer", std::vector<bool>(lower.size(), false));
    if (lower.empty() || upper.size() != lower.size() || whole.size() != lower.size())
        return err_response("lower, upper and integer: one each per parameter");
    for (std::size_t d = 0; d < lower.size(); ++d) {
        if (!(lower[d] <= upper[d])) return err_response("lower above upper");
    }
    std::vector<std::vector<double>> tried;
    std::vector<double> values;
    const json& t = body.contains("tried") ? body["tried"] : json::array();
    const json& v = body.contains("values") ? body["values"] : json::array();
    if (!t.is_array() || !v.is_array() || t.size() != v.size())
        return err_response("tried and values: one value per point tried");
    for (std::size_t i = 0; i < t.size(); ++i) {
        if (!t[i].is_array() || t[i].size() != lower.size())
            return err_response("tried: each point a value per parameter");
        tried.push_back(t[i].get<std::vector<double>>());
        values.push_back(v[i].is_number() ? v[i].get<double>() : std::numeric_limits<double>::quiet_NaN());
    }
    const int batch = std::clamp(jv(body, "batch", 8), 1, 512);
    std::vector<char> integer(whole.begin(), whole.end());
    const auto points = bayes_next(lower, upper, integer, tried, values, batch, jv<std::uint64_t>(body, "seed", 0));
    return ok_response({{"points", points}}, json::object(), t0_ms);
}

std::string run_ito_json(const std::string& src, double t0_ms) {
    const auto req = parse_request(src);
    if (!req) return req.error();
    const json& j = *req;
    const auto f = jv<std::string>(j, "function_type", "exp_martingale");
    const ItoCheck c = ito_check(ito_code(f), jv(j, "theta", 0.7), jv(j, "t", 1.0), jv(j, "n_steps", 2000));
    json summary = {{"function_type", f},           {"value", c.value},
                    {"target_expectation", c.target}, {"std_error", c.std_error},
                    {"formula_residual", c.residual}, {"paths", c.paths}};
    return ok_response(std::move(summary), json::object(), t0_ms);
}

std::string run_measure_density_json(const std::string& src, double t0_ms) {
    const auto req = parse_request(src);
    if (!req) return req.error();
    const json& j = *req;
    const int n = std::max(20, jv(j, "n_steps", 100));
    std::vector<double> density(static_cast<std::size_t>(n + 1));
    const int length =
        measure_density_path(jv(j, "mu", 0.05), jv(j, "r", 0.02), jv(j, "sigma", 0.2), jv(j, "t", 1.0), n, density);
    density.resize(static_cast<std::size_t>(std::max(0, length)));
    json summary = {{"final_density", density.empty() ? 1.0 : density.back()},
                    {"length", static_cast<int>(density.size())}};
    return ok_response(std::move(summary), {{"density", density}}, t0_ms);
}

std::string run_measure_compare_json(const std::string& src, double t0_ms) {
    const auto req = parse_request(src);
    if (!req) return req.error();
    const json& j = *req;
    const int n_steps = jv(j, "n_steps", 100);
    const int pl = std::max(1, std::min(jv(j, "preview_len", 50), n_steps));
    std::vector<double> st(10, 0.0), preview(static_cast<std::size_t>(2 * pl), 0.0);
    measure_compare(jv(j, "mu", 0.05), jv(j, "r", 0.02), jv(j, "sigma", 0.2), jv(j, "t", 1.0), n_steps,
                    jv(j, "n_paths", 1000), jv(j, "x0", 1.0), st, preview, pl);
    json summary = {
        {"p_stats", {{"mean", st[0]}, {"variance", st[1]}, {"q05", st[2]}, {"q50", st[3]}, {"q95", st[4]}}},
        {"q_stats", {{"mean", st[5]}, {"variance", st[6]}, {"q05", st[7]}, {"q50", st[8]}, {"q95", st[9]}}},
    };
    const std::vector<double> p(preview.begin(), preview.begin() + pl), q(preview.begin() + pl, preview.end());
    return ok_response(std::move(summary), {{"path_preview", {{"P", p}, {"Q", q}}}}, t0_ms);
}

std::string run_pde_json(const std::string& src, double t0_ms) {
    const auto req = parse_request(src, {"spot", "strike", "rate", "vol", "maturity"},
                                   "missing_required_pde_fields");
    if (!req) return req.error();
    const json& j = *req;
    const auto method = jv<std::string>(j, "method", "crank_nicolson");
    PdeSpec spec;
    spec.spot = jv(j, "spot", 0.0);
    spec.strike = jv(j, "strike", 0.0);
    spec.rate = jv(j, "rate", 0.0);
    spec.dividend_yield = jv(j, "dividend_yield", 0.0);
    spec.vol = jv(j, "vol", 0.0);
    spec.maturity = jv(j, "maturity", 1.0);
    spec.s_steps = jv(j, "s_steps", 100);
    spec.t_steps = jv(j, "t_steps", 100);
    spec.method = method == "implicit" ? 1 : method == "explicit" ? 2 : 0;
    spec.is_call = jv<std::string>(j, "option_type", "call") != "put";
    spec.american = jv(j, "is_american", false);
    const PdeOutcome o = pde_solve(spec);
    json summary = {{"price", o.price},
                    {"method", spec.method == 1 ? "implicit" : spec.method == 2 ? "explicit" : "crank_nicolson"},
                    {"s_steps", spec.s_steps},
                    {"t_steps", spec.t_steps},
                    {"t_steps_used", o.t_steps_used},
                    {"stability_refined", o.refined},
                    {"psor_iterations", o.psor_iterations},
                    {"option_type", spec.is_call ? "call" : "put"},
                    {"is_american", spec.american}};
    return ok_response(std::move(summary), json::object(), t0_ms);
}

// The vol at one strike and expiry, bilinear on the grid given (nearest when it has one row or column).
std::string run_vol_surface_json(const std::string& src, double t0_ms) {
    const auto req = parse_request(src, {"spot", "target_strike", "target_expiry"},
                                   "missing_required_vol_surface_fields");
    if (!req) return req.error();
    const json& j = *req;
    for (const std::string k : {"strikes", "expiries", "ivs"}) {
        if (!j.contains(k) || !j[k].is_array()) return err_response("missing_vol_surface_arrays");
    }
    const double spot = jv(j, "spot", 0.0), strike = jv(j, "target_strike", 0.0);
    const double expiry = jv(j, "target_expiry", 0.0);
    const auto strikes = j["strikes"].get<std::vector<double>>();
    const auto expiries = j["expiries"].get<std::vector<double>>();
    const auto ivs = j["ivs"].get<std::vector<double>>();
    double iv = 0.0;
    std::string method = "error";
    if (!strikes.empty() && spot > 0.0 && strike > 0.0 && expiry > 0.0) {
        const double v = vol_surface_interp(strikes, expiries, ivs, spot, strike, expiry);
        if (v >= 0.0) {
            iv = v;
            std::vector<double> um = strikes, uv = expiries;
            std::ranges::sort(um);
            um.erase(std::ranges::unique(um).begin(), um.end());
            std::ranges::sort(uv);
            uv.erase(std::ranges::unique(uv).begin(), uv.end());
            method = um.size() >= 2 && uv.size() >= 2 ? "cpp_bilinear" : "cpp_nearest";
        }
    }
    return ok_response({{"iv", iv}, {"method", method}, {"n_points", static_cast<int>(strikes.size())}},
                       json::object(), t0_ms);
}

}  // namespace quant::analytics
