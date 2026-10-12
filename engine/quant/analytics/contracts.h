#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace quant::analytics {

// ── Unified task response envelope ───────────────────────────────────────────
// Returned by ResponseAssembler, serialized to JSON by json_codec.

// The version of every response's shape. Bump it when a response changes; the server checks it on load
// (quant_contract_version) and reports a stale build instead of misreading it.
inline constexpr const char* kContractVersion = "2.0";

enum class TaskStatus { Ok, Error };
enum class GateDecision { Go, Warn, Block };

struct TaskResponse {
    std::string   contract_version = kContractVersion;
    std::string   trace_id;
    TaskStatus    status     = TaskStatus::Ok;
    GateDecision  decision   = GateDecision::Go;
    std::string   task_type;
    double        runtime_ms = 0.0;
    std::string   error_code;
    std::string   error_message;
};



// ── Pricing ──────────────────────────────────────────────────────────────────

struct PricingResult {
    double mc         = 0.0;
    double bs         = 0.0;
    double binomial   = 0.0;
    double mc_std_err = 0.0;
    double american   = 0.0;          // = american_binomial (kept for older clients)
    bool   has_american = false;
    double trinomial  = 0.0;
    bool   is_call    = true;
    double american_binomial  = 0.0;
    double american_trinomial = 0.0;
    double american_pde       = 0.0;  // Crank-Nicolson + PSOR
    double american_lsm       = 0.0;
    double american_lsm_stderr = 0.0;
    int    american_lsm_paths  = 0;
    long long american_pde_psor_iterations = 0;
    double delta_bs   = 0.0;
    double gamma_bs   = 0.0;
    double theta_bs   = 0.0;
    double vega_bs    = 0.0;
    double rho_bs     = 0.0;
    double mc_minus_bs      = 0.0;
    double binomial_minus_bs = 0.0;
    double spread     = 0.0;
    double rel_spread = 0.0;
    std::string stability;   // "good" | "check_model_params"
    std::string numeraire;
    // Echo of input params for display
    double in_spot     = 0.0;
    double in_strike   = 0.0;
    double in_vol      = 0.0;
    double in_maturity = 0.0;
    double in_rate     = 0.0;
    int    in_n_paths  = 0;
};

struct BatchJobResult {
    double spot    = 0.0;
    double strike  = 0.0;
    double vol     = 0.0;
    double mc      = 0.0;
    double bs      = 0.0;
    double binomial = 0.0;
    double spread  = 0.0;
};

struct MethodStats {
    std::string method;
    double avg = 0.0;
    double min = 0.0;
    double max = 0.0;
    double std = 0.0;
    double avg_err_vs_bs = 0.0;
};

struct PricingBatchResult {
    int    job_count    = 0;
    double runtime_ms   = 0.0;
    double avg_spread   = 0.0;
    double p50_spread   = 0.0;
    double p95_spread   = 0.0;
    double max_spread   = 0.0;
    double min_spread   = 0.0;
    double spread_std   = 0.0;
    double spread_cv    = 0.0;
    int    worst_idx    = -1;
    int    best_idx     = -1;
    std::vector<BatchJobResult> rows;
    std::vector<MethodStats>    method_summary;
};

// ── Scenario ─────────────────────────────────────────────────────────────────

struct ScenarioRow {
    std::string name;
    double bs_price       = 0.0;
    double vs_base_diff   = 0.0;
    double abs_diff       = 0.0;
    double vs_base_pct    = 0.0;    // NaN signals "not available"
    double vs_base_bps    = 0.0;
    int    rank           = 0;
    bool   has_pct        = false;
};

struct ScenarioResult {
    double base_price  = 0.0;
    double max_price   = 0.0;
    double min_price   = 0.0;
    std::vector<ScenarioRow> rows;
};

// ── Hedging ──────────────────────────────────────────────────────────────────

struct HistogramData {
    std::vector<double> edges;
    std::vector<double> counts;
};

struct StrategyStats {
    std::string name;
    double mean             = 0.0;
    double std              = 0.0;
    double q05              = 0.0;
    double q50              = 0.0;
    double q95              = 0.0;
    double turnover         = 0.0;
    double transaction_cost = 0.0;
    double var95            = 0.0;
    double es95             = 0.0;
};

struct HedgingResult {
    double pnl_mean = 0.0;
    double pnl_std  = 0.0;
    double pnl_q05  = 0.0;
    double pnl_q50  = 0.0;
    double pnl_q95  = 0.0;
    // Derived tail analytics
    double normalized_std          = 0.0;
    double tail_span               = 0.0;
    double left_tail               = 0.0;
    double right_tail              = 0.0;
    double tail_skew_proxy         = 0.0;
    double tail_ratio_q95_q05      = 0.0;
    double scaling_proxy_std_sqrt_n = 0.0;
    // Run parameters (for display)
    int    n_rebalances = 0;
    int    n_paths      = 0;
    int    n_legs       = 1;
    double horizon      = 0.0;   // years: the longest leg
    HistogramData histogram;
    std::vector<StrategyStats> strategies;
    std::string best_strategy;
    double compare_tc_bps           = 0.0;
    double compare_threshold        = 0.0;
    double compare_vol_mismatch_mult = 0.0;
};

// ── Validation ───────────────────────────────────────────────────────────────

struct ValidationRow {
    std::string capability;
    std::string metric;
    double value     = 0.0;
    double threshold = 0.0;
    double excess    = 0.0;
    std::string status;          // "pass" | "fail"
    std::string interpretation;
    std::string action;
};

struct ValidationResult {
    std::string gate_decision;   // "go" | "warning" | "blocked"
    std::string mode;            // "advisory" | "blocking"
    int    checks_total  = 0;
    int    checks_failed = 0;
    double fail_rate     = 0.0;
    double max_excess    = 0.0;
    std::string max_excess_item;
    std::string blocking_reason;
    std::vector<ValidationRow> rows;
};

// ── Multi-Leg ─────────────────────────────────────────────────────────────────

struct LegResult {
    std::string option_type;  // "call" | "put"
    double      strike       = 0.0;
    double      quantity     = 0.0;   // signed: positive = long, negative = short
    double      bs_price     = 0.0;
    double      mc_price     = 0.0;
    double      binomial_price = 0.0;  // European CRR tree, 250 steps a year (at least 10)
    double      delta_bs     = 0.0;
    double      vega_bs      = 0.0;
    double      vol          = 0.0;   // effective per-leg inputs
    double      maturity     = 0.0;
    double      forward      = 0.0;   // 0 when the leg is priced off spot
};

struct MultiLegResult {
    std::vector<LegResult> legs;
    double net_bs_price  = 0.0;
    double net_mc_price  = 0.0;
    double net_binomial_price = 0.0;
    double net_delta     = 0.0;
    double net_vega      = 0.0;
    std::string strategy_hint;
};

// ── Calibration ───────────────────────────────────────────────────────────────

struct ImpliedVolResult {
    double implied_vol  = -1.0;
    double final_error  = 0.0;
    bool   converged    = false;
};

struct ImpliedVolBatchResult {
    std::vector<double>   ivs;
    std::vector<uint8_t>  converged;  // 1=converged, 0=failed (avoids vector<bool> bitfield)
    int    n_converged  = 0;
    double runtime_ms   = 0.0;
};

struct HestonCalibrationResult {
    double v0     = 0.04;
    double kappa  = 1.5;
    double theta  = 0.04;
    double xi     = 0.5;
    double rho    = -0.7;
    double rmse   = 0.0;
    double max_abs_error = 0.0;
    int    iterations    = 0;
    bool   converged     = false;
    std::vector<double> model_prices;
    std::vector<double> residuals;
    double runtime_ms    = 0.0;
};

// ── Greek Surface ─────────────────────────────────────────────────────────────

struct GreekSurfaceResult {
    std::string greek_name;
    std::vector<double> spots;
    std::vector<double> maturities;
    std::vector<double> grid;        // row-major: spots × maturities
    double grid_min   = 0.0;
    double grid_max   = 0.0;
    double strike     = 0.0;
    double vol        = 0.0;
    double rate       = 0.0;
    std::string contract_version = kContractVersion;
    std::string trace_id;
    double runtime_ms = 0.0;
};

// ── Screener ──────────────────────────────────────────────────────────────────

struct ScreenedLeg {
    std::string symbol;
    std::string option_type;   // "call" | "put"
    std::string expiry;
    double strike      = 0.0;
    double years       = 0.0;
    double forward     = 0.0;
    int    qty         = 0;    // +1 buy / -1 sell (per contract)
    double fill_price  = 0.0;  // price used for cost (ask/bid or mid), per 1 unit
    double mark        = 0.0;
    double iv          = 0.0;
    double model_price = 0.0;  // per 1 unit; NaN if no model
    double delta = 0.0, gamma = 0.0, theta = 0.0, vega = 0.0;
};

struct ScreenedStrategy {
    std::string kind;          // single | iron_condor | straddle | strangle | forward_vol
    std::string direction;     // LONG | SHORT
    std::string label;
    std::vector<ScreenedLeg> legs;
    double debit = 0.0, credit = 0.0, cost = 0.0;
    double max_gain = 0.0, max_loss = 0.0, rr = 0.0;
    double net_delta = 0.0, net_gamma = 0.0, net_theta = 0.0, net_vega = 0.0;
    double avg_iv = 0.0, model_value = 0.0, edge = 0.0, forward_vol = 0.0;
};

struct ScreenerKindCount {
    std::string kind;
    long long   generated = 0;
    long long   passed    = 0;
};

struct ScreenerResult {
    std::vector<ScreenedStrategy>  top;
    std::vector<ScreenerKindCount> by_kind;
    int       n_options_in           = 0;
    int       n_options_after_filter = 0;
    long long n_generated            = 0;
    long long n_passed               = 0;
    double    runtime_ms             = 0.0;
};

}  // namespace quant::analytics
