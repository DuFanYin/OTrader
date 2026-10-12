#include "engine.h"
#include "quant/kernel.h"
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <optional>
#include <chrono>
#include <expected>
#include <ranges>
#include <string_view>

namespace quant::analytics {

using json = nlohmann::json;

// ── Internal helpers ──────────────────────────────────────────────────────────

namespace {

constexpr double kZ95 = 1.96;
constexpr int kSimulationPreviewCap = 200;

double elapsed_ms(double t0_ms) {
    return static_cast<double>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()
        ).count()
    ) / 1000.0 - t0_ms;
}

template<typename T>
T jv(const json& j, std::string_view k, T def) {
    const std::string key(k);
    if (j.contains(key) && !j[key].is_null()) { try { return j[key].get<T>(); } catch(...){} }
    return def;
}

json base_envelope(const std::string& trace_id, const std::string& decision = "go",
                   const std::string& severity = "info") {
    return {
        {"contract_version", kContractVersion},
        {"trace_id",         trace_id},
        {"status",           "ok"},
        {"decision",         decision},
        {"severity",         severity},
        {"validation", {
            {"checks_total", 0}, {"checks_failed", 0},
            {"failed_items", json::array()}, {"failed_by_capability", json::object()}
        }},
        {"actions", json::array()},
        {"strategy_recommendation", json::object()},
        {"error", nullptr},
    };
}

json diag_block(double runtime_ms, const std::string& compute_path) {
    return {
        {"runtime_ms",        runtime_ms},
        {"stage_timings_ms",  {{"total", runtime_ms}}},
        {"engine_version",    kContractVersion},
        {"compute_path",      json::array({compute_path})},
    };
}

std::string err_response(const std::string& trace_id, const std::string& message) {
    return json{
        {"contract_version", kContractVersion}, {"trace_id", trace_id},
        {"status", "error"}, {"decision", "block"}, {"severity", "high"},
        {"result_summary", json::object()}, {"result_details", json::object()},
        {"validation", {{"checks_total",0},{"checks_failed",0},
                        {"failed_items",json::array()},{"failed_by_capability",json::object()}}},
        {"actions", json::array()}, {"strategy_recommendation", json::object()},
        {"diagnostics", json::object()},
        {"error", {{"code", "bad_request"}, {"message", message}}},
    }.dump();
}

std::expected<json, std::string> parse_request(const std::string& src) {
    try {
        return json::parse(src);
    } catch (...) {
        return std::unexpected(err_response("no-trace", "invalid_json"));
    }
}

std::expected<void, std::string> require_fields(
    const json& j,
    const std::string& trace_id,
    std::initializer_list<std::string_view> keys,
    std::string_view error_code
) {
    const bool ok = std::ranges::all_of(keys, [&](std::string_view key) {
        return j.contains(std::string(key));
    });
    if (!ok) {
        return std::unexpected(err_response(trace_id, std::string(error_code)));
    }
    return {};
}

struct RequestContext {
    json body;
    std::string trace_id;
};

std::expected<RequestContext, std::string> prepare_request(
    const std::string& src,
    std::initializer_list<std::string_view> required = {},
    std::string_view missing_error = ""
) {
    auto parsed = parse_request(src);
    if (!parsed) return std::unexpected(parsed.error());
    RequestContext ctx{std::move(parsed.value()), "no-trace"};
    ctx.trace_id = jv<std::string>(ctx.body, "trace_id", "no-trace");
    if (!required.size()) return ctx;
    auto check = require_fields(ctx.body, ctx.trace_id, required, missing_error);
    if (!check) return std::unexpected(check.error());
    return ctx;
}

PricingParams parse_pricing_params(const json& j) {
    return PricingParams{
        jv<double>(j, "spot", 0.0), jv<double>(j, "strike", 0.0), jv<double>(j, "rate", 0.0),
        jv<double>(j, "vol", 0.0), jv<double>(j, "maturity", 0.0), jv<double>(j, "dividend_yield", 0.0),
        jv<int>(j, "n_paths", 10000), jv<int>(j, "n_steps", 0), jv<bool>(j, "is_american", false),
        jv<bool>(j, "fx_mode", false), jv<std::string>(j, "product_type", "european_call"),
        jv<std::string>(j, "numeraire", "money_market"),
        jv<std::string>(j, "mc_sampler", "pseudorandom"),
        jv<std::string>(j, "option_type", ""),
        jv<int>(j, "lsm_paths", 0), jv<int>(j, "lsm_steps", 50)
    };
}

BatchGridParams parse_batch_grid_params(const json& j) {
    return BatchGridParams{
        parse_pricing_params(j),
        jv<int>(j, "n_jobs", 16),
        jv<double>(j, "spot_shock", 0.02)
    };
}

std::vector<LegSpec> parse_legs(const json& j, double default_strike) {
    std::vector<LegSpec> legs;
    if (!j.contains("legs") || !j["legs"].is_array()) return legs;
    for (const auto& leg : j["legs"]) {
        LegSpec ls;
        ls.option_type = jv<std::string>(leg, "option_type", "call");
        ls.strike      = jv<double>(leg, "strike", default_strike);
        ls.quantity    = jv<double>(leg, "quantity", 1.0);
        ls.vol         = jv<double>(leg, "vol", 0.0);
        ls.maturity    = jv<double>(leg, "maturity", 0.0);
        ls.forward     = jv<double>(leg, "forward", 0.0);
        legs.push_back(ls);
    }
    return legs;
}

HedgingParams parse_hedging_params(const json& j) {
    return HedgingParams{
        .spot = jv<double>(j, "spot", 0.0), .strike = jv<double>(j, "strike", 0.0), .rate = jv<double>(j, "rate", 0.0),
        .vol = jv<double>(j, "vol", 0.0), .maturity = jv<double>(j, "maturity", 0.0),
        .n_rebalances = jv<int>(j, "n_rebalances", 52), .n_paths = jv<int>(j, "n_paths", 2000), .n_bins = jv<int>(j, "n_bins", 40),
        .transaction_cost_bps = jv<double>(j, "transaction_cost_bps", 5.0),
        .rebalance_threshold = jv<double>(j, "rebalance_threshold", 0.02),
        .vol_mismatch_mult = jv<double>(j, "vol_mismatch_mult", 1.15),
        .dividend_yield = jv<double>(j, "dividend_yield", 0.0), .option_type = jv<std::string>(j, "option_type", "call"),
        .legs = parse_legs(j, jv<double>(j, "spot", 0.0)),
    };
}

ScenarioParams parse_scenario_params(const json& j) {
    return ScenarioParams{
        jv<double>(j, "spot", 0.0), jv<double>(j, "strike", 0.0), jv<double>(j, "rate", 0.0),
        jv<double>(j, "vol", 0.0), jv<double>(j, "maturity", 0.0), jv<double>(j, "dividend_yield", 0.0),
        jv<std::string>(j, "option_type", "call")
    };
}

ValidationParams parse_validation_params(const json& j) {
    return ValidationParams{
        jv<double>(j, "spot", 0.0), jv<double>(j, "strike", 0.0), jv<double>(j, "rate", 0.0),
        jv<double>(j, "vol", 0.0), jv<double>(j, "maturity", 0.0), jv<double>(j, "mu", 0.0),
        jv<bool>(j, "pick_stats", false), jv<bool>(j, "pick_ito", false), jv<bool>(j, "pick_simulation", false),
        jv<bool>(j, "compute_block_on_validation", false), jv<double>(j, "stats_theta", 1.0), jv<int>(j, "stats_n", 10000),
        jv<double>(j, "ito_theta", 0.7), jv<double>(j, "ito_t", 1.0), jv<int>(j, "ito_n", 2000),
        jv<std::string>(j, "ito_function_type", "exp_martingale"), jv<int>(j, "sim_steps", 100),
        jv<int>(j, "n_rebalances", 52), jv<int>(j, "hedge_paths", 500),
        jv<bool>(j, "pick_lattice", false)
    };
}

SimulationParams parse_simulation_params(const json& j) {
    return SimulationParams{
        jv<std::string>(j, "model", "gbm"), jv<int>(j, "n_steps", 100), jv<double>(j, "dt", 0.01),
        jv<double>(j, "sigma", 0.2), jv<double>(j, "kappa", 1.2), jv<double>(j, "theta", 0.03), jv<double>(j, "x0", 1.0)
    };
}

StatsParams parse_stats_params(const json& j) {
    return StatsParams{
        jv<double>(j, "mu", 0.0), jv<double>(j, "sigma", 1.0), jv<double>(j, "theta", 1.0), jv<int>(j, "sample_size", 10000)
    };
}

ItoParams parse_ito_params(const json& j) {
    return ItoParams{
        jv<std::string>(j, "function_type", "exp_martingale"), jv<double>(j, "theta", 0.7),
        jv<double>(j, "t", 1.0), jv<int>(j, "n_steps", 2000)
    };
}

MeasureDensityParams parse_measure_density_params(const json& j) {
    return MeasureDensityParams{
        jv<double>(j, "mu", 0.05), jv<double>(j, "r", 0.02), jv<double>(j, "sigma", 0.2),
        jv<double>(j, "t", 1.0), jv<int>(j, "n_steps", 100)
    };
}

MeasureCompareParams parse_measure_compare_params(const json& j) {
    return MeasureCompareParams{
        jv<double>(j, "mu", 0.05), jv<double>(j, "r", 0.02), jv<double>(j, "sigma", 0.2), jv<double>(j, "t", 1.0),
        jv<int>(j, "n_steps", 100), jv<int>(j, "n_paths", 1000), jv<double>(j, "x0", 1.0), jv<int>(j, "preview_len", 50)
    };
}

PdeParams parse_pde_params(const json& j) {
    return PdeParams{
        jv<double>(j, "spot", 0.0), jv<double>(j, "strike", 0.0), jv<double>(j, "rate", 0.0),
        jv<double>(j, "vol", 0.0), jv<double>(j, "maturity", 1.0), jv<double>(j, "dividend_yield", 0.0),
        jv<int>(j, "s_steps", 100), jv<int>(j, "t_steps", 100), jv<std::string>(j, "method", "crank_nicolson"),
        jv<std::string>(j, "option_type", "call"), jv<bool>(j, "is_american", false)
    };
}

// ── Serializers ───────────────────────────────────────────────────────────────

std::string ser_pricing(const std::string& tid, const PricingResult& r, double ms) {
    json summary = {
        {"mc", r.mc}, {"bs", r.bs}, {"binomial", r.binomial},
        {"mc_std_err", r.mc_std_err},
        {"mc_ci_low",  r.mc - kZ95 * r.mc_std_err},
        {"mc_ci_high", r.mc + kZ95 * r.mc_std_err},
        {"american",   r.has_american ? json(r.american) : json(nullptr)},
        {"option_type", r.is_call ? "call" : "put"},
        {"trinomial",  r.trinomial},
        {"trinomial_minus_bs", r.trinomial - r.bs},
        {"american_methods", r.has_american ? json{
            {"binomial", r.american_binomial}, {"trinomial", r.american_trinomial},
            {"pde_psor", r.american_pde}, {"pde_psor_iterations", r.american_pde_psor_iterations},
            {"lsm", r.american_lsm}, {"lsm_std_err", r.american_lsm_stderr}, {"lsm_paths", r.american_lsm_paths},
            {"early_exercise_premium", r.american_binomial - r.binomial},
        } : json(nullptr)},
        {"method_spread", r.spread}, {"relative_spread", r.rel_spread},
        {"pricing_stability", r.stability}, {"numeraire", r.numeraire},
        {"greeks", {{"delta_bs", r.delta_bs}, {"gamma_bs", r.gamma_bs},
                    {"theta_bs", r.theta_bs}, {"vega_bs",  r.vega_bs},
                    {"rho_bs",   r.rho_bs}}},
        {"error_decomposition", {{"mc_minus_bs", r.mc_minus_bs},
                                  {"binomial_minus_bs", r.binomial_minus_bs}}},
        {"abs_mc_bs", std::abs(r.mc_minus_bs)},
        {"abs_binomial_bs", std::abs(r.binomial_minus_bs)},
    };
    json j = base_envelope(tid);
    j["input_params"]   = {{"spot",r.in_spot},{"strike",r.in_strike},{"vol",r.in_vol},
                            {"maturity",r.in_maturity},{"rate",r.in_rate},{"n_paths",r.in_n_paths}};
    j["summary"] = j["result_summary"] = summary;
    j["details"]  = j["result_details"] = json::object();
    j["diagnostics"] = diag_block(ms, "pricing");
    return j.dump();
}

std::string ser_batch(const std::string& tid, const PricingBatchResult& r) {
    json flat = json::array();
    for (int i = 0; i < (int)r.rows.size(); ++i) {
        const auto& row = r.rows[i];
        flat.push_back({{"#",i},{"spot",row.spot},{"strike",row.strike},{"vol",row.vol},
                         {"bs",row.bs},{"mc",row.mc},{"binomial",row.binomial},
                         {"mc-bs",row.mc-row.bs},{"bin-bs",row.binomial-row.bs},{"spread",row.spread}});
    }
    json msummary = json::array();
    for (const auto& m : r.method_summary)
        msummary.push_back({{"method",m.method},{"avg",m.avg},{"min",m.min},
                             {"max",m.max},{"std",m.std},{"avg_err_vs_bs",m.avg_err_vs_bs}});
    json summary = {
        {"job_count",r.job_count},{"total_compute_ms",r.runtime_ms},
        {"avg_method_spread",r.avg_spread},{"p50_method_spread",r.p50_spread},
        {"p95_method_spread",r.p95_spread},{"max_method_spread",r.max_spread},
        {"min_method_spread",r.min_spread},
        {"avg_runtime_per_job_ms", r.job_count>0 ? r.runtime_ms/r.job_count : 0.0},
        {"worst_spread_job_index",r.worst_idx},{"best_spread_job_index",r.best_idx},
        {"spread_std",r.spread_std},{"spread_cv",r.spread_cv},
    };
    json details = {{"flat_rows",flat},{"method_summary",msummary}};
    json j = base_envelope(tid);
    j["summary"] = j["result_summary"] = summary;
    j["details"]  = j["result_details"] = details;
    j["diagnostics"] = diag_block(r.runtime_ms, "pricing_batch");
    return j.dump();
}

std::string ser_scenario(const std::string& tid, const ScenarioResult& r, double ms) {
    json rows = json::array(), tornado = json::array();
    for (const auto& row : r.rows) {
        rows.push_back({{"rank",row.rank},{"scenario",row.name},{"base_price",r.base_price},
                         {"bs_price",row.bs_price},{"vs_base_diff",row.vs_base_diff},
                         {"abs_vs_base_diff",row.abs_diff},
                         {"vs_base_pct", row.has_pct ? json(row.vs_base_pct) : json(nullptr)},
                         {"vs_base_bps", row.has_pct ? json(row.vs_base_bps) : json(nullptr)}});
        if (row.name != "base")
            tornado.push_back({{"label",row.name},{"value",row.vs_base_diff}});
    }
    json summary = {{"base_price",r.base_price},{"max_price",r.max_price},{"min_price",r.min_price},
                     {"price_range",r.max_price-r.min_price},{"scenario_count",(int)r.rows.size()}};
    json details = {{"rows",rows},{"tornado_points",tornado}};
    json j = base_envelope(tid);
    j["summary"] = j["result_summary"] = summary;
    j["details"]  = j["result_details"] = details;
    j["diagnostics"] = diag_block(ms, "scenario");
    return j.dump();
}

std::string ser_hedging(const std::string& tid, const HedgingResult& r, double ms) {
    json sc = json::object();
    const StrategyStats* best = nullptr;
    for (const auto& s : r.strategies) {
        sc[s.name] = {{"mean",s.mean},{"std",s.std},{"q05",s.q05},{"q50",s.q50},{"q95",s.q95},
                       {"turnover",s.turnover},{"transaction_cost",s.transaction_cost},
                       {"var95",s.var95},{"es95",s.es95}};
        if (s.name == r.best_strategy) best = &s;
    }
    if (!best && !r.strategies.empty()) best = &r.strategies[0];
    auto best_j = [&]() -> json {
        return best ? json{{"name",best->name},{"reason","min_es95_then_std_then_mean"},
                           {"es95",best->es95},{"std",best->std},{"mean",best->mean}}
                    : json::object();
    };
    json summary = {
        {"strategy","delta_hedge"},{"n_rebalances",r.n_rebalances},{"n_paths",r.n_paths},
        {"pnl_mean",r.pnl_mean},{"pnl_std",r.pnl_std},
        {"pnl_q05",r.pnl_q05},{"pnl_q50",r.pnl_q50},{"pnl_q95",r.pnl_q95},
        {"normalized_std",r.normalized_std},{"tail_span",r.tail_span},
        {"left_tail",r.left_tail},{"right_tail",r.right_tail},
        {"tail_skew_proxy",r.tail_skew_proxy},{"tail_ratio_q95_q05",r.tail_ratio_q95_q05},
        {"scaling_proxy_std_sqrt_n",r.scaling_proxy_std_sqrt_n},{"best_strategy",r.best_strategy},
        {"n_legs",r.n_legs},{"horizon",r.horizon},
    };
    json details = {
        {"histogram",{{"edges",r.histogram.edges},{"counts",r.histogram.counts}}},
        {"strategy_compare",sc}, {"best_strategy",best_j()},
        {"compare_config",{{"transaction_cost_bps",r.compare_tc_bps},
                            {"rebalance_threshold",r.compare_threshold},
                            {"vol_mismatch_mult",r.compare_vol_mismatch_mult}}},
    };
    json j = base_envelope(tid);
    j["summary"] = j["result_summary"] = summary;
    j["details"]  = j["result_details"] = details;
    j["strategy_recommendation"] = best ? json{{"name",best->name},
        {"reason","min_es95_then_std_then_mean"},
        {"metrics",{{"es95",best->es95},{"std",best->std},{"mean",best->mean}}}}
        : json::object();
    j["diagnostics"] = diag_block(ms, "hedging");
    return j.dump();
}

std::string ser_validation(const std::string& tid, const ValidationResult& r, double ms) {
    json rows = json::array();
    for (const auto& row : r.rows)
        rows.push_back({{"capability",row.capability},{"metric",row.metric},
                         {"value",row.value},{"threshold",row.threshold},{"excess",row.excess},
                         {"status",row.status},{"interpretation",row.interpretation},{"action",row.action}});
    json summary = {{"validation_mode",r.mode},{"gate_decision",r.gate_decision},
                     {"checks_total",r.checks_total},{"checks_failed",r.checks_failed},
                     {"fail_rate",r.fail_rate},{"failed_by_capability",json::object()},
                     {"max_excess",r.max_excess},{"max_excess_item",r.max_excess_item}};
    json details = {{"rows",rows},{"threshold_rows",rows}};
    json j = base_envelope(tid,
        r.gate_decision == "go" ? "go" : "warn",
        r.checks_failed > 0 ? "medium" : "info");
    j["summary"] = j["result_summary"] = summary;
    j["details"]  = j["result_details"] = details;
    j["validation"] = {{"checks_total",r.checks_total},{"checks_failed",r.checks_failed},
                        {"failed_items",json::array()},{"failed_by_capability",json::object()}};
    j["diagnostics"] = diag_block(ms, "validation_gate");
    return j.dump();
}

}  // namespace

// ── Public endpoint functions (called from c_api.cpp) ────────────────────────

std::string run_pricing_json(const std::string& src, double t0_ms) {
    const auto ctx = prepare_request(src, {"spot", "strike", "rate", "vol", "maturity"},
                                     "missing_required_pricing_fields");
    if (!ctx) return ctx.error();
    return ser_pricing(ctx->trace_id, run_pricing(parse_pricing_params(ctx->body)), elapsed_ms(t0_ms));
}

std::string run_pricing_batch_grid_json(const std::string& src, double t0_ms) {
    const auto ctx = prepare_request(src, {"spot", "strike", "rate", "vol", "maturity"},
                                     "missing_required_pricing_batch_fields");
    if (!ctx) return ctx.error();
    return ser_batch(ctx->trace_id, run_pricing_batch_grid(parse_batch_grid_params(ctx->body), elapsed_ms(t0_ms)));
}

std::string run_pricing_batch_json(const std::string& src, double t0_ms) {
    const auto req = parse_request(src);
    if (!req) return req.error();
    const auto& j = req.value();
    const std::string tid = jv<std::string>(j,"trace_id","no-trace");
    int n = std::max(1, std::min(jv<int>(j,"n_jobs",0), 500));
    if (n == 0) return err_response(tid, "missing_required_pricing_batch_fields");
    std::vector<PricingParams> jobs(n);
    for (int i = 0; i < n; ++i) {
        const std::string idx = std::to_string(i);
        auto get = [&](const std::string& f) {
            const std::string k = "job_"+idx+"_"+f;
            return j.contains(k) ? j[k].get<double>() : 0.0;
        };
        jobs[i] = {get("spot"),get("strike"),get("rate"),get("vol"),get("maturity"),
                   get("dividend_yield"), jv<int>(j,"job_"+idx+"_n_paths",10000)};
        if (jobs[i].spot==0.0||jobs[i].strike==0.0)
            return err_response(tid, "missing_required_pricing_batch_fields");
    }
    return ser_batch(tid, run_pricing_batch(jobs, elapsed_ms(t0_ms)));
}

std::string run_hedging_json(const std::string& src, double t0_ms) {
    const auto ctx = prepare_request(src, {"spot", "strike", "rate", "vol", "maturity"},
                                     "missing_required_hedging_fields");
    if (!ctx) return ctx.error();
    return ser_hedging(ctx->trace_id, run_hedging(parse_hedging_params(ctx->body)), elapsed_ms(t0_ms));
}

std::string run_scenario_json(const std::string& src, double t0_ms) {
    const auto ctx = prepare_request(src, {"spot", "strike", "rate", "vol", "maturity"},
                                     "missing_required_scenario_fields");
    if (!ctx) return ctx.error();
    return ser_scenario(ctx->trace_id, run_scenario(parse_scenario_params(ctx->body)), elapsed_ms(t0_ms));
}

std::string run_validation_json(const std::string& src, double t0_ms) {
    const auto ctx = prepare_request(src, {"spot", "strike", "rate", "vol", "maturity"},
                                     "missing_required_validation_fields");
    if (!ctx) return ctx.error();
    return ser_validation(ctx->trace_id, run_validation(parse_validation_params(ctx->body)), elapsed_ms(t0_ms));
}

std::string run_simulation_json(const std::string& src, double t0_ms) {
    const auto ctx = prepare_request(src);
    if (!ctx) return ctx.error();
    const auto r = run_simulation(parse_simulation_params(ctx->body));
    double x_min = r.values.empty() ? 0.0 : *std::ranges::min_element(r.values);
    double x_max = r.values.empty() ? 0.0 : *std::ranges::max_element(r.values);
    const int stride = (int)r.values.size() > kSimulationPreviewCap
        ? (int)r.values.size() / kSimulationPreviewCap : 1;
    json preview = json::array();
    for (size_t i = 0; i < r.values.size(); i += stride) preview.push_back(r.values[i]);
    json jj = base_envelope(ctx->trace_id);
    jj["result_summary"] = {{"model",r.model},{"n_steps",r.n_steps},
                              {"x_final",r.values.empty()?0.0:r.values.back()},
                              {"x_min",x_min},{"x_max",x_max},{"length",(int)r.values.size()}};
    jj["result_details"] = {{"values_preview",preview}};
    jj["diagnostics"]    = diag_block(elapsed_ms(t0_ms), "simulation");
    return jj.dump();
}

std::string run_stats_json(const std::string& src, double t0_ms) {
    const auto ctx = prepare_request(src);
    if (!ctx) return ctx.error();
    const auto r = run_stats(parse_stats_params(ctx->body));
    json jj = base_envelope(ctx->trace_id);
    jj["result_summary"] = {{"mgf",r.mgf},{"mean",r.mean},{"variance",r.variance},{"mgf_exact",r.mgf_exact},
                              {"mean_exact",r.mean_exact},{"variance_exact",r.variance_exact}};
    jj["diagnostics"]    = diag_block(elapsed_ms(t0_ms), "stats");
    return jj.dump();
}

std::string run_ito_json(const std::string& src, double t0_ms) {
    const auto ctx = prepare_request(src);
    if (!ctx) return ctx.error();
    const auto r = run_ito(parse_ito_params(ctx->body));
    json jj = base_envelope(ctx->trace_id);
    jj["result_summary"] = {{"function_type",r.function_type},{"value",r.value},
                              {"target_expectation",r.target},{"std_error",r.std_error},
                              {"formula_residual",r.residual},{"paths",r.paths}};
    jj["diagnostics"] = diag_block(elapsed_ms(t0_ms), "ito_check");
    return jj.dump();
}

std::string run_measure_density_json(const std::string& src, double t0_ms) {
    const auto ctx = prepare_request(src);
    if (!ctx) return ctx.error();
    const auto r = run_measure_density(parse_measure_density_params(ctx->body));
    json jj = base_envelope(ctx->trace_id);
    jj["result_summary"] = {{"final_density", r.density.empty()?1.0:r.density.back()},
                              {"length",(int)r.density.size()}};
    jj["result_details"] = {{"density", r.density}};
    jj["diagnostics"]    = diag_block(elapsed_ms(t0_ms), "measure_density");
    return jj.dump();
}

std::string run_measure_compare_json(const std::string& src, double t0_ms) {
    const auto ctx = prepare_request(src);
    if (!ctx) return ctx.error();
    const auto r = run_measure_compare(parse_measure_compare_params(ctx->body));
    json jj = base_envelope(ctx->trace_id);
    jj["result_summary"] = {
        {"p_stats",{{"mean",r.p_mean},{"variance",r.p_var},{"q05",r.p_q05},{"q50",r.p_q50},{"q95",r.p_q95}}},
        {"q_stats",{{"mean",r.q_mean},{"variance",r.q_var},{"q05",r.q_q05},{"q50",r.q_q50},{"q95",r.q_q95}}},
    };
    jj["result_details"] = {{"path_preview",{{"P",r.preview_p},{"Q",r.preview_q}}}};
    jj["diagnostics"]    = diag_block(elapsed_ms(t0_ms), "measure_compare");
    return jj.dump();
}

std::string run_pde_json(const std::string& src, double t0_ms) {
    const auto ctx = prepare_request(src, {"spot", "strike", "rate", "vol", "maturity"},
                                     "missing_required_pde_fields");
    if (!ctx) return ctx.error();
    const auto r = run_pde(parse_pde_params(ctx->body));
    json jj = base_envelope(ctx->trace_id);
    jj["result_summary"] = {{"price",r.price},{"method",r.method},
                              {"s_steps",r.s_steps},{"t_steps",r.t_steps},
                              {"t_steps_used",r.t_steps_used},{"stability_refined",r.refined},
                              {"psor_iterations",r.psor_iterations},
                              {"option_type",r.option_type},{"is_american",r.is_american}};
    jj["diagnostics"] = diag_block(elapsed_ms(t0_ms), "pde");
    return jj.dump();
}

std::string run_greek_surface_json(const std::string& src, double t0_ms) {
    const auto ctx = prepare_request(src, {"strike", "rate", "vol", "spot_min", "spot_max"},
                                     "missing_required_greek_surface_fields");
    if (!ctx) return ctx.error();
    const auto& j = ctx->body;
    GreekSurfaceParams p;
    p.strike         = jv<double>(j, "strike",         0.0);
    p.rate           = jv<double>(j, "rate",           0.0);
    p.vol            = jv<double>(j, "vol",            0.2);
    p.dividend_yield = jv<double>(j, "dividend_yield", 0.0);
    p.spot_min       = jv<double>(j, "spot_min",       0.0);
    p.spot_max       = jv<double>(j, "spot_max",       0.0);
    p.mat_min        = jv<double>(j, "mat_min",        0.1);
    p.mat_max        = jv<double>(j, "mat_max",        3.0);
    p.n_spots        = jv<int>   (j, "n_spots",        21);
    p.n_mats         = jv<int>   (j, "n_mats",         11);
    p.greek          = jv<std::string>(j, "greek",     "delta");
    const auto r     = run_greek_surface(p);
    json jj = base_envelope(ctx->trace_id);
    jj["result_summary"] = {
        {"greek",      r.greek_name},
        {"n_spots",    (int)r.spots.size()},
        {"n_mats",     (int)r.maturities.size()},
        {"grid_min",   r.grid_min},
        {"grid_max",   r.grid_max},
        {"strike",     r.strike},
        {"vol",        r.vol},
        {"rate",       r.rate},
    };
    jj["result_details"] = {
        {"spots",      r.spots},
        {"maturities", r.maturities},
        {"grid",       r.grid},
    };
    jj["diagnostics"] = diag_block(elapsed_ms(t0_ms), "greek_surface");
    return jj.dump();
}

std::string run_multi_leg_json(const std::string& src, double t0_ms) {
    const auto ctx = prepare_request(src, {"spot", "rate", "vol", "maturity", "legs"},
                                     "missing_required_multi_leg_fields");
    if (!ctx) return ctx.error();
    const auto& j = ctx->body;
    MultiLegParams p;
    p.spot           = jv<double>(j, "spot",           0.0);
    p.rate           = jv<double>(j, "rate",           0.0);
    p.vol            = jv<double>(j, "vol",            0.2);
    p.maturity       = jv<double>(j, "maturity",       1.0);
    p.dividend_yield = jv<double>(j, "dividend_yield", 0.0);
    p.n_paths        = jv<int>   (j, "n_paths",        10000);
    p.legs = parse_legs(j, p.spot);
    if (p.legs.empty()) return err_response(ctx->trace_id, "legs_array_empty");
    const auto r = run_multi_leg(p);
    json legs_arr = json::array();
    for (const auto& lr : r.legs) {
        legs_arr.push_back({
            {"option_type", lr.option_type}, {"strike", lr.strike}, {"quantity", lr.quantity},
            {"bs_price", lr.bs_price}, {"mc_price", lr.mc_price}, {"binomial_price", lr.binomial_price},
            {"delta_bs", lr.delta_bs}, {"vega_bs", lr.vega_bs},
            {"vol", lr.vol}, {"maturity", lr.maturity}, {"forward", lr.forward},
        });
    }
    json jj = base_envelope(ctx->trace_id);
    jj["result_summary"] = {
        {"net_bs_price",  r.net_bs_price},
        {"net_mc_price",  r.net_mc_price},
        {"net_binomial_price", r.net_binomial_price},
        {"net_delta",     r.net_delta},
        {"net_vega",      r.net_vega},
        {"strategy_hint", r.strategy_hint},
        {"n_legs",        (int)r.legs.size()},
    };
    jj["result_details"] = {{"legs", legs_arr}};
    jj["diagnostics"] = diag_block(elapsed_ms(t0_ms), "multi_leg");
    return jj.dump();
}

std::string run_implied_vol_json(const std::string& src, double t0_ms) {
    const auto ctx = prepare_request(src,
        {"market_price", "spot", "strike", "rate", "maturity"},
        "missing_required_implied_vol_fields");
    if (!ctx) return ctx.error();
    const auto& j = ctx->body;
    ImpliedVolParams p;
    p.market_price   = jv<double>(j, "market_price",   0.0);
    p.spot           = jv<double>(j, "spot",           0.0);
    p.strike         = jv<double>(j, "strike",         0.0);
    p.rate           = jv<double>(j, "rate",           0.0);
    p.maturity       = jv<double>(j, "maturity",       1.0);
    p.dividend_yield = jv<double>(j, "dividend_yield", 0.0);
    const auto r = run_implied_vol(p);
    json jj = base_envelope(ctx->trace_id);
    jj["result_summary"] = {
        {"implied_vol",  r.implied_vol},
        {"converged",    r.converged},
        {"final_error",  r.final_error},
    };
    jj["diagnostics"] = diag_block(elapsed_ms(t0_ms), "implied_vol");
    return jj.dump();
}

std::string run_implied_vol_batch_json(const std::string& src, double t0_ms) {
    const auto ctx = prepare_request(src, {"spot", "rate"},
                                     "missing_required_implied_vol_batch_fields");
    if (!ctx) return ctx.error();
    const auto& j = ctx->body;
    ImpliedVolBatchParams p;
    p.spot           = jv<double>(j, "spot",           0.0);
    p.rate           = jv<double>(j, "rate",           0.0);
    p.dividend_yield = jv<double>(j, "dividend_yield", 0.0);
    for (const std::string arr : {"market_prices", "strikes", "expiries"}) {
        if (!j.contains(arr) || !j[arr].is_array())
            return err_response(ctx->trace_id, "missing_array_" + arr);
    }
    p.market_prices = j["market_prices"].get<std::vector<double>>();
    p.strikes       = j["strikes"].get<std::vector<double>>();
    p.expiries      = j["expiries"].get<std::vector<double>>();
    if (p.market_prices.size() != p.strikes.size() || p.strikes.size() != p.expiries.size())
        return err_response(ctx->trace_id, "array_length_mismatch");
    const auto r = run_implied_vol_batch(p);
    json ivs_arr = r.ivs;
    json conv_arr = json::array();
    for (uint8_t c : r.converged) conv_arr.push_back(static_cast<bool>(c));
    json jj = base_envelope(ctx->trace_id);
    jj["result_summary"] = {
        {"n_points",    (int)r.ivs.size()},
        {"n_converged", r.n_converged},
        {"convergence_rate", r.ivs.empty() ? 0.0
            : static_cast<double>(r.n_converged) / r.ivs.size()},
    };
    jj["result_details"] = {{"ivs", ivs_arr}, {"converged", conv_arr}};
    jj["diagnostics"] = diag_block(elapsed_ms(t0_ms), "implied_vol_batch");
    return jj.dump();
}

// Black on each option's forward, discounted at `rate`: the trading engine's pricing (quant::black_*).
// With "vols": each price and its Greeks (to "spots", the forward when absent); with "prices": each
// implied vol (0: none).
std::string run_black_batch_json(const std::string& src, double t0_ms) {
    const auto ctx = prepare_request(src, {"rate"}, "missing_required_black_batch_fields");
    if (!ctx) return ctx.error();
    const auto& j = ctx->body;
    for (const std::string arr : {"is_call", "forwards", "strikes", "maturities"}) {
        if (!j.contains(arr) || !j[arr].is_array())
            return err_response(ctx->trace_id, "missing_array_" + arr);
    }
    const bool pricing = j.contains("vols");
    if (pricing == j.contains("prices"))
        return err_response(ctx->trace_id, "give_vols_or_prices");
    const auto is_call = j["is_call"].get<std::vector<bool>>();
    const auto forwards = j["forwards"].get<std::vector<double>>();
    const auto strikes = j["strikes"].get<std::vector<double>>();
    const auto maturities = j["maturities"].get<std::vector<double>>();
    const auto values = j[pricing ? "vols" : "prices"].get<std::vector<double>>();
    const auto spots = j.contains("spots") ? j["spots"].get<std::vector<double>>() : forwards;
    const std::size_t n = is_call.size();
    if (forwards.size() != n || strikes.size() != n || maturities.size() != n || values.size() != n ||
        spots.size() != n)
        return err_response(ctx->trace_id, "array_length_mismatch");
    const double rate = jv<double>(j, "rate", 0.0);
    json jj = base_envelope(ctx->trace_id);
    if (pricing) {
        std::vector<double> price(n), delta(n), gamma(n), theta(n), vega(n);
        for (std::size_t i = 0; i < n; ++i) {
            price[i] = quant::black_price(is_call[i], forwards[i], strikes[i], maturities[i], rate, values[i]);
            const auto g = quant::black_greeks(is_call[i], spots[i], forwards[i], strikes[i], maturities[i],
                                               rate, values[i]);
            delta[i] = g.delta;
            gamma[i] = g.gamma;
            theta[i] = g.theta;
            vega[i] = g.vega;
        }
        jj["result_summary"] = {{"n_points", (int)n}, {"mode", "price"}};
        jj["result_details"] = {{"prices", price}, {"delta", delta}, {"gamma", gamma}, {"theta", theta},
                                {"vega", vega}};
    } else {
        std::vector<double> ivs(n);
        for (std::size_t i = 0; i < n; ++i)
            ivs[i] = quant::black_implied_vol(values[i], forwards[i], strikes[i], maturities[i], rate, is_call[i]);
        jj["result_summary"] = {{"n_points", (int)n}, {"mode", "implied_vol"}};
        jj["result_details"] = {{"ivs", ivs}};
    }
    jj["diagnostics"] = diag_block(elapsed_ms(t0_ms), "black_batch");
    return jj.dump();
}

std::string run_heston_price_json(const std::string& src, double t0_ms) {
    const auto ctx = prepare_request(src, {"spot", "strike", "rate", "maturity"},
                                     "missing_required_heston_price_fields");
    if (!ctx) return ctx.error();
    const auto& j = ctx->body;
    HestonPriceParams p;
    p.spot    = jv<double>(j, "spot",    0.0);
    p.strike  = jv<double>(j, "strike",  0.0);
    p.rate    = jv<double>(j, "rate",    0.0);
    p.maturity = jv<double>(j, "maturity", 1.0);
    p.v0     = jv<double>(j, "v0",    0.04);
    p.kappa  = jv<double>(j, "kappa", 1.5);
    p.theta  = jv<double>(j, "theta", 0.04);
    p.xi     = jv<double>(j, "xi",    0.5);
    p.rho    = jv<double>(j, "rho",   -0.7);
    const double price = run_heston_price(p);
    json jj = base_envelope(ctx->trace_id);
    jj["result_summary"] = {{"heston_price", price}};
    jj["diagnostics"] = diag_block(elapsed_ms(t0_ms), "heston_price");
    return jj.dump();
}

std::string run_heston_calibrate_json(const std::string& src, double t0_ms) {
    const auto ctx = prepare_request(src, {"spot", "rate"},
                                     "missing_required_heston_calibrate_fields");
    if (!ctx) return ctx.error();
    const auto& j = ctx->body;
    HestonCalibrationParams p;
    p.spot           = jv<double>(j, "spot",           0.0);
    p.rate           = jv<double>(j, "rate",           0.0);
    p.dividend_yield = jv<double>(j, "dividend_yield", 0.0);
    p.init_v0    = jv<double>(j, "init_v0",    0.04);
    p.init_kappa = jv<double>(j, "init_kappa", 1.5);
    p.init_theta = jv<double>(j, "init_theta", 0.04);
    p.init_xi    = jv<double>(j, "init_xi",    0.5);
    p.init_rho   = jv<double>(j, "init_rho",   -0.7);
    p.max_iter   = jv<int>   (j, "max_iter",   500);
    for (const std::string arr : {"market_strikes", "market_maturities", "market_prices"}) {
        if (!j.contains(arr) || !j[arr].is_array())
            return err_response(ctx->trace_id, "missing_array_" + arr);
    }
    p.market_strikes    = j["market_strikes"].get<std::vector<double>>();
    p.market_maturities = j["market_maturities"].get<std::vector<double>>();
    p.market_prices     = j["market_prices"].get<std::vector<double>>();
    if (p.market_prices.empty())
        return err_response(ctx->trace_id, "calibration_data_empty");
    const auto r = run_heston_calibrate(p);
    json jj = base_envelope(ctx->trace_id);
    jj["result_summary"] = {
        {"v0",           r.v0},    {"kappa",        r.kappa},
        {"theta",        r.theta}, {"xi",           r.xi},
        {"rho",          r.rho},   {"rmse",         r.rmse},
        {"max_abs_error", r.max_abs_error},
        {"iterations",   r.iterations},
        {"converged",    r.converged},
    };
    jj["result_details"] = {
        {"model_prices", r.model_prices},
        {"residuals",    r.residuals},
    };
    jj["diagnostics"] = diag_block(elapsed_ms(t0_ms), "heston_calibrate");
    return jj.dump();
}

std::string run_vol_surface_json(const std::string& src, double t0_ms) {
    const auto ctx = prepare_request(src, {"spot", "target_strike", "target_expiry"},
                                     "missing_required_vol_surface_fields");
    if (!ctx) return ctx.error();
    const auto& j = ctx->body;
    for (std::string_view key : {"strikes", "expiries", "ivs"}) {
        const std::string k(key);
        if (!j.contains(k) || !j[k].is_array()) return err_response(ctx->trace_id, "missing_vol_surface_arrays");
    }
    VolSurfaceParams p;
    p.spot=jv<double>(j,"spot",0.0); p.target_strike=jv<double>(j,"target_strike",0.0);
    p.target_expiry=jv<double>(j,"target_expiry",0.0);
    p.strikes=j["strikes"].get<std::vector<double>>();
    p.expiries=j["expiries"].get<std::vector<double>>();
    p.ivs=j["ivs"].get<std::vector<double>>();
    const auto r = run_vol_surface(p);
    json jj = base_envelope(ctx->trace_id);
    jj["result_summary"] = {{"iv",r.iv},{"method",r.method},{"n_points",(int)p.strikes.size()}};
    jj["diagnostics"]    = diag_block(elapsed_ms(t0_ms), "vol_surface");
    return jj.dump();
}

// ── Screener ──────────────────────────────────────────────────────────────────

namespace {

struct ScreenerParseError { std::string code; };

double jnum_or_nan(const json& j, const char* key) {
    if (!j.contains(key) || !j[key].is_number()) return std::numeric_limits<double>::quiet_NaN();
    return j[key].get<double>();
}

std::optional<double> jopt_num(const json& j, const char* key) {
    if (!j.contains(key) || j[key].is_null()) return std::nullopt;
    if (!j[key].is_number()) throw ScreenerParseError{std::string("bad_number_") + key};
    return j[key].get<double>();
}

// [lo, hi] -> active range; null -> inactive. Either bound may be null (open-ended).
ScreenRange jrange(const json& j, const char* key) {
    ScreenRange r;
    if (!j.contains(key) || j[key].is_null()) return r;
    const json& a = j[key];
    if (!a.is_array() || a.size() != 2) throw ScreenerParseError{std::string("bad_range_") + key};
    const auto bound = [&](const json& v, double open) {
        if (v.is_null()) return open;
        if (!v.is_number()) throw ScreenerParseError{std::string("bad_range_") + key};
        return v.get<double>();
    };
    r.on = true;
    r.lo = bound(a[0], -std::numeric_limits<double>::infinity());
    r.hi = bound(a[1],  std::numeric_limits<double>::infinity());
    return r;
}

ChainOption parse_chain_option(const json& o) {
    ChainOption c;
    std::string type = jv<std::string>(o, "option_type", "");
    std::ranges::transform(type, type.begin(), [](unsigned char ch) { return std::tolower(ch); });
    if (type != "call" && type != "put") throw ScreenerParseError{"bad_option_type"};
    c.is_call = type == "call";
    c.symbol  = jv<std::string>(o, "symbol", "");
    c.expiry  = jv<std::string>(o, "expiry", "");
    c.strike  = jv<double>(o, "strike", 0.0);
    c.forward = jv<double>(o, "forward", 0.0);
    c.years   = jv<double>(o, "years", 0.0);
    c.bid     = jnum_or_nan(o, "bid");
    c.ask     = jnum_or_nan(o, "ask");
    c.mark    = jv<double>(o, "mark", 0.0);
    c.iv      = jv<double>(o, "iv", 0.0);
    c.volume  = jv<double>(o, "volume", 0.0);
    c.oi      = jv<double>(o, "oi", 0.0);
    c.delta   = jv<double>(o, "delta", 0.0);
    c.gamma   = jv<double>(o, "gamma", 0.0);
    c.theta   = jv<double>(o, "theta", 0.0);
    c.vega    = jv<double>(o, "vega", 0.0);
    if (c.strike <= 0.0) throw ScreenerParseError{"bad_strike"};
    return c;
}

ScreenRankKey parse_rank_key(const std::string& k) {
    if (k == "rr")          return ScreenRankKey::RR;
    if (k == "gain")        return ScreenRankKey::Gain;
    if (k == "loss")        return ScreenRankKey::Loss;
    if (k == "cost")        return ScreenRankKey::Cost;
    if (k == "credit")      return ScreenRankKey::Credit;
    if (k == "edge")        return ScreenRankKey::Edge;
    if (k == "forward_vol") return ScreenRankKey::ForwardVol;
    throw ScreenerParseError{"bad_rank_key"};
}

ScreenerParams parse_screener_params(const json& j) {
    ScreenerParams p;
    p.spot           = jv<double>(j, "spot", 0.0);
    p.rate           = jv<double>(j, "rate", 0.0);
    p.multiplier     = jv<double>(j, "multiplier", 1.0);
    p.compute_greeks = jv<bool>(j, "compute_greeks", true);
    if (p.multiplier <= 0.0) throw ScreenerParseError{"bad_multiplier"};

    const auto price_mode = jv<std::string>(j, "price_mode", "executable");
    if (price_mode == "executable")  p.price_mode = ScreenPriceMode::Executable;
    else if (price_mode == "mid")    p.price_mode = ScreenPriceMode::Mid;
    else throw ScreenerParseError{"bad_price_mode"};

    p.model_vol = jv<std::string>(j, "model_vol", "mark");
    if (p.model_vol == "flat") {
        p.model_vol_flat = jv<double>(j, "model_vol_flat", 0.0);
        if (p.model_vol_flat <= 0.0) throw ScreenerParseError{"model_vol_flat_required"};
    } else if (p.model_vol == "surface") {
        const json s = j.value("surface", json::object());
        for (const char* k : {"strikes", "expiries", "ivs"})
            if (!s.contains(k) || !s[k].is_array()) throw ScreenerParseError{"surface_arrays_required"};
        p.surface_strikes  = s["strikes"].get<std::vector<double>>();
        p.surface_expiries = s["expiries"].get<std::vector<double>>();
        p.surface_ivs      = s["ivs"].get<std::vector<double>>();
        if (p.surface_strikes.empty() || p.surface_strikes.size() != p.surface_expiries.size()
            || p.surface_strikes.size() != p.surface_ivs.size() || p.spot <= 0.0)
            throw ScreenerParseError{"bad_surface"};
    } else if (p.model_vol == "heston") {
        const json h = j.value("heston", json::object());
        p.heston.v0    = jv<double>(h, "v0",    p.heston.v0);
        p.heston.kappa = jv<double>(h, "kappa", p.heston.kappa);
        p.heston.theta = jv<double>(h, "theta", p.heston.theta);
        p.heston.xi    = jv<double>(h, "xi",    p.heston.xi);
        p.heston.rho   = jv<double>(h, "rho",   p.heston.rho);
    } else if (p.model_vol != "mark" && p.model_vol != "none") {
        throw ScreenerParseError{"bad_model_vol"};
    }

    if (!j["chain"].is_array()) throw ScreenerParseError{"chain_must_be_array"};
    p.chain.reserve(j["chain"].size());
    for (const auto& o : j["chain"]) p.chain.push_back(parse_chain_option(o));

    const json st = j.value("strategies", json::object());
    p.strategies.single_calls = jv<bool>(st, "single_calls", false);
    p.strategies.iron_condors = jv<bool>(st, "iron_condors", false);
    p.strategies.straddles    = jv<bool>(st, "straddles",    false);
    p.strategies.strangles    = jv<bool>(st, "strangles",    false);
    p.strategies.forward_vols = jv<bool>(st, "forward_vols", false);

    const json of = j.value("option_filter", json::object());
    auto& f = p.option_filter;
    f.min_volume             = jopt_num(of, "min_volume");
    f.min_oi                 = jopt_num(of, "min_oi");
    f.min_price              = jopt_num(of, "min_price");
    f.max_bid_ask_spread     = jopt_num(of, "max_bid_ask_spread");
    f.max_bid_ask_spread_pct = jopt_num(of, "max_bid_ask_spread_pct");
    if (of.contains("expiry") && of["expiry"].is_string()) f.expiry = of["expiry"].get<std::string>();
    f.days_to_expiry    = jrange(of, "days_to_expiry_range");
    f.volume_ratio      = jrange(of, "volume_ratio_range");
    f.moneyness         = jrange(of, "moneyness_range");
    f.require_two_sided = jv<bool>(of, "require_two_sided", true);

    const json sf_ = j.value("strategy_filter", json::object());
    auto& g = p.strategy_filter;
    const auto direction = jv<std::string>(sf_, "direction", "LONG");
    if (direction != "LONG" && direction != "SHORT") throw ScreenerParseError{"bad_direction"};
    g.long_direction = direction == "LONG";
    g.debit       = jrange(sf_, "debit_range");
    g.credit      = jrange(sf_, "credit_range");
    g.gain        = jrange(sf_, "potential_gain_range");
    g.loss        = jrange(sf_, "potential_loss_range");
    g.rr          = jrange(sf_, "rr_range");
    g.net_delta   = jrange(sf_, "net_delta_range");
    g.net_theta   = jrange(sf_, "net_theta_range");
    g.net_vega    = jrange(sf_, "net_vega_range");
    g.iv          = jrange(sf_, "iv_range");
    g.forward_vol = jrange(sf_, "forward_vol_range");
    g.edge        = jrange(sf_, "edge_range");

    const json rk = j.value("rank", json::object());
    p.rank.key = parse_rank_key(jv<std::string>(rk, "key", "rr"));
    // Natural order: smallest loss / cost first, largest of everything else first.
    const bool natural_desc = p.rank.key != ScreenRankKey::Loss && p.rank.key != ScreenRankKey::Cost;
    p.rank.descending = jv<bool>(rk, "descending", natural_desc);
    p.rank.top_n      = std::clamp(jv<int>(rk, "top_n", 20), 1, 500);
    return p;
}

json screened_leg_json(const ScreenedLeg& l) {
    return {
        {"symbol", l.symbol}, {"option_type", l.option_type}, {"expiry", l.expiry},
        {"strike", l.strike}, {"years", l.years}, {"forward", l.forward}, {"qty", l.qty},
        {"fill_price", l.fill_price}, {"mark", l.mark}, {"iv", l.iv}, {"model_price", l.model_price},
        {"delta", l.delta}, {"gamma", l.gamma}, {"theta", l.theta}, {"vega", l.vega},
    };
}

}  // namespace

std::string run_screener_json(const std::string& src, double t0_ms) {
    const auto ctx = prepare_request(src, {"spot", "chain"}, "missing_required_screener_fields");
    if (!ctx) return ctx.error();
    ScreenerParams p;
    try {
        p = parse_screener_params(ctx->body);
    } catch (const ScreenerParseError& e) {
        return err_response(ctx->trace_id, e.code);
    } catch (const json::exception&) {
        return err_response(ctx->trace_id, "bad_screener_request");
    }
    const std::string model_vol  = p.model_vol;
    const double      multiplier = p.multiplier;
    const bool        mid_mode   = p.price_mode == ScreenPriceMode::Mid;
    const std::string rank_key   = jv<std::string>(ctx->body.value("rank", json::object()), "key", "rr");
    const bool        rank_desc  = p.rank.descending;

    const auto r = run_screener(std::move(p));

    json by_kind = json::object();
    for (const auto& k : r.by_kind) by_kind[k.kind] = {{"generated", k.generated}, {"passed", k.passed}};

    json strategies = json::array();
    for (size_t i = 0; i < r.top.size(); ++i) {
        const auto& s = r.top[i];
        json legs = json::array();
        for (const auto& l : s.legs) legs.push_back(screened_leg_json(l));
        // Non-finite values (unbounded gain/loss, missing model) serialize as null.
        strategies.push_back({
            {"rank", static_cast<int>(i) + 1}, {"kind", s.kind}, {"direction", s.direction},
            {"label", s.label}, {"legs", legs},
            {"debit", s.debit}, {"credit", s.credit}, {"cost", s.cost},
            {"max_gain", s.max_gain}, {"max_loss", s.max_loss}, {"rr", s.rr},
            {"max_gain_unbounded", std::isinf(s.max_gain)}, {"max_loss_unbounded", std::isinf(s.max_loss)},
            {"net_delta", s.net_delta}, {"net_gamma", s.net_gamma},
            {"net_theta", s.net_theta}, {"net_vega", s.net_vega},
            {"avg_iv", s.avg_iv}, {"model_value", s.model_value}, {"edge", s.edge},
            {"forward_vol", s.forward_vol},
        });
    }

    json jj = base_envelope(ctx->trace_id);
    jj["result_summary"] = {
        {"n_options_in",           r.n_options_in},
        {"n_options_after_filter", r.n_options_after_filter},
        {"n_generated",            r.n_generated},
        {"n_passed",               r.n_passed},
        {"n_returned",             static_cast<int>(r.top.size())},
        {"by_kind",                by_kind},
        {"rank_key",               rank_key},
        {"rank_descending",        rank_desc},
        {"model_vol",              model_vol},
        {"price_mode",             mid_mode ? "mid" : "executable"},
        {"multiplier",             multiplier},
        {"engine_runtime_ms",      r.runtime_ms},
    };
    jj["result_details"] = {{"strategies", strategies}};
    jj["diagnostics"]    = diag_block(elapsed_ms(t0_ms), "screener");
    return jj.dump();
}

}  // namespace quant::analytics
