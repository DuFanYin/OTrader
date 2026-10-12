#pragma once

#include "contracts.h"
#include "quant/screener/screener.h"
#include <string>
#include <vector>

namespace quant::analytics {

// ── PricingEngine ─────────────────────────────────────────────────────────────

struct PricingParams {
    double spot          = 0.0;
    double strike        = 0.0;
    double rate          = 0.0;
    double vol           = 0.0;
    double maturity      = 0.0;
    double dividend_yield = 0.0;
    int    n_paths       = 10000;
    int    n_steps       = 0;    // 0 = auto (maturity * 250)
    bool   is_american   = false;
    bool   fx_mode       = false;
    std::string product_type = "european_call";
    std::string numeraire    = "money_market";
    std::string mc_sampler   = "pseudorandom";  // "pseudorandom" | "antithetic" | "sobol"
    std::string option_type  = "";   // "call" | "put"; "" -> derived from product_type
    int    lsm_paths     = 0;        // 0 = min(max(n_paths, 1000), 200000)
    int    lsm_steps     = 50;
};

struct BatchGridParams {
    PricingParams base;
    int    n_jobs     = 16;
    double spot_shock = 0.02;
};

struct GreekSurfaceParams {
    double strike         = 0.0;
    double rate           = 0.0;
    double vol            = 0.0;
    double dividend_yield = 0.0;
    double spot_min       = 0.0;
    double spot_max       = 0.0;
    double mat_min        = 0.1;
    double mat_max        = 3.0;
    int    n_spots        = 21;
    int    n_mats         = 11;
    std::string greek     = "delta";
};

struct LegSpec {
    std::string option_type = "call";  // "call" | "put"
    double strike   = 0.0;
    double quantity = 1.0;  // signed notional (positive=long, negative=short)
    // Optional per-leg overrides; <= 0 means "use the strategy-level value".
    double vol      = 0.0;
    double maturity = 0.0;
    double forward  = 0.0;  // price this leg off its own forward (Black-76 equivalent)
};

struct MultiLegParams {
    double spot          = 0.0;
    double rate          = 0.0;
    double vol           = 0.0;
    double maturity      = 0.0;
    double dividend_yield = 0.0;
    int    n_paths       = 10000;
    std::vector<LegSpec> legs;
};

PricingResult      run_pricing(const PricingParams& p);
PricingBatchResult run_pricing_batch(const std::vector<PricingParams>& jobs, double runtime_ms);
PricingBatchResult run_pricing_batch_grid(const BatchGridParams& p, double runtime_ms);
GreekSurfaceResult run_greek_surface(const GreekSurfaceParams& p);
MultiLegResult     run_multi_leg(const MultiLegParams& p);

// ── ScenarioEngine ────────────────────────────────────────────────────────────

struct ScenarioParams {
    double spot           = 0.0;
    double strike         = 0.0;
    double rate           = 0.0;
    double vol            = 0.0;
    double maturity       = 0.0;
    double dividend_yield = 0.0;
    std::string option_type = "call";
};

ScenarioResult run_scenario(const ScenarioParams& p);

// ── HedgingEngine ─────────────────────────────────────────────────────────────

struct HedgingParams {
    double spot                  = 0.0;
    double strike                = 0.0;
    double rate                  = 0.0;
    double vol                   = 0.0;
    double maturity              = 0.0;
    int    n_rebalances          = 52;
    int    n_paths               = 2000;
    int    n_bins                = 40;
    double transaction_cost_bps  = 5.0;
    double rebalance_threshold   = 0.02;
    double vol_mismatch_mult     = 1.15;
    double dividend_yield        = 0.0;
    std::string option_type      = "call";   // without legs: one such option sold at strike
    std::vector<LegSpec> legs;               // a position to hedge instead
};

HedgingResult run_hedging(const HedgingParams& p);

// ── ValidationEngine ──────────────────────────────────────────────────────────

struct ValidationParams {
    double spot    = 0.0;
    double strike  = 0.0;
    double rate    = 0.0;
    double vol     = 0.0;
    double maturity = 0.0;
    double mu      = 0.0;
    bool   pick_stats      = false;
    bool   pick_ito        = false;
    bool   pick_simulation = false;
    bool   block_mode      = false;
    double stats_theta     = 1.0;
    int    stats_n         = 10000;
    double ito_theta       = 0.7;
    double ito_t           = 1.0;
    int    ito_n           = 2000;
    std::string ito_function_type = "exp_martingale";
    int    sim_steps       = 100;
    int    n_rebalances    = 52;
    int    hedge_paths     = 500;
    bool   pick_lattice    = false;   // trinomial vs BS, LSM / PDE vs binomial American
};

ValidationResult run_validation(const ValidationParams& p);

// ── UtilityEngine ─────────────────────────────────────────────────────────────

struct SimulationParams {
    std::string model = "gbm";
    int    n_steps = 100;
    double dt      = 0.01;
    double sigma   = 0.2;
    double kappa   = 1.2;
    double theta   = 0.03;
    double x0      = 1.0;
};

struct StatsParams {
    double mu          = 0.0;
    double sigma       = 1.0;
    double theta       = 1.0;
    int    sample_size = 10000;
};

struct ItoParams {
    std::string function_type = "exp_martingale";
    double theta  = 0.7;
    double t      = 1.0;
    int    n_steps = 2000;
};

struct MeasureDensityParams {
    double mu      = 0.05;
    double r       = 0.02;
    double sigma   = 0.2;
    double t       = 1.0;
    int    n_steps = 100;
};

struct MeasureCompareParams {
    double mu       = 0.05;
    double r        = 0.02;
    double sigma    = 0.2;
    double t        = 1.0;
    int    n_steps  = 100;
    int    n_paths  = 1000;
    double x0       = 1.0;
    int    preview_len = 50;
};

struct PdeParams {
    double spot           = 0.0;
    double strike         = 0.0;
    double rate           = 0.0;
    double vol            = 0.0;
    double maturity       = 1.0;
    double dividend_yield = 0.0;
    int    s_steps        = 100;
    int    t_steps        = 100;
    std::string method    = "crank_nicolson";   // crank_nicolson | implicit | explicit
    std::string option_type = "call";
    bool   is_american    = false;
};

struct SimulationResult {
    std::string     model;
    int             n_steps = 0;
    std::vector<double> values;
};

struct StatsResult {   // sample moments, with the exact ones they estimate
    double mgf      = 0.0;
    double mean     = 0.0;
    double variance = 0.0;
    double mgf_exact      = 0.0;
    double mean_exact     = 0.0;
    double variance_exact = 0.0;
};

struct ItoResult {
    std::string function_type;
    double value     = 0.0;
    double target    = 0.0;
    double std_error = 0.0;
    double residual  = 0.0;
    int    paths     = 0;
};

struct MeasureDensityResult {
    std::vector<double> density;
};

struct MeasureCompareResult {
    double p_mean = 0.0, p_var = 0.0, p_q05 = 0.0, p_q50 = 0.0, p_q95 = 0.0;
    double q_mean = 0.0, q_var = 0.0, q_q05 = 0.0, q_q50 = 0.0, q_q95 = 0.0;
    std::vector<double> preview_p;
    std::vector<double> preview_q;
};

struct PdeResult {
    double      price   = 0.0;
    std::string method;
    int         s_steps = 0;
    int         t_steps = 0;
    int         t_steps_used = 0;     // > t_steps when the explicit scheme was refined for stability
    bool        refined = false;
    long long   psor_iterations = 0;
    std::string option_type;
    bool        is_american = false;
};

struct VolSurfaceParams {
    std::vector<double> strikes;
    std::vector<double> expiries;
    std::vector<double> ivs;
    double spot           = 0.0;
    double target_strike  = 0.0;
    double target_expiry  = 0.0;
};

struct VolSurfaceResult {
    double iv            = 0.0;
    std::string method;   // "cpp_bilinear" | "cpp_nearest"
};

// ── CalibrationEngine ─────────────────────────────────────────────────────────

struct ImpliedVolParams {
    double market_price  = 0.0;
    double spot          = 0.0;
    double strike        = 0.0;
    double rate          = 0.0;
    double maturity      = 0.0;
    double dividend_yield = 0.0;
};

struct ImpliedVolBatchParams {
    std::vector<double> market_prices;
    std::vector<double> strikes;
    std::vector<double> expiries;
    double spot          = 0.0;
    double rate          = 0.0;
    double dividend_yield = 0.0;
};

struct HestonCalibrationParams {
    double spot           = 0.0;
    double rate           = 0.0;
    double dividend_yield = 0.0;
    std::vector<double> market_strikes;
    std::vector<double> market_maturities;
    std::vector<double> market_prices;
    double init_v0    = 0.04;
    double init_kappa = 1.5;
    double init_theta = 0.04;
    double init_xi    = 0.5;
    double init_rho   = -0.7;
    int    max_iter   = 500;
};

struct HestonPriceParams {
    double spot     = 0.0;
    double strike   = 0.0;
    double rate     = 0.0;
    double maturity = 0.0;
    double v0       = 0.04;
    double kappa    = 1.5;
    double theta    = 0.04;
    double xi       = 0.5;
    double rho      = -0.7;
};

ImpliedVolResult          run_implied_vol        (const ImpliedVolParams& p);
ImpliedVolBatchResult     run_implied_vol_batch  (const ImpliedVolBatchParams& p);
HestonCalibrationResult   run_heston_calibrate   (const HestonCalibrationParams& p);
double                    run_heston_price       (const HestonPriceParams& p);

SimulationResult     run_simulation      (const SimulationParams& p);
StatsResult          run_stats           (const StatsParams& p);
ItoResult            run_ito             (const ItoParams& p);
MeasureDensityResult run_measure_density (const MeasureDensityParams& p);
MeasureCompareResult run_measure_compare (const MeasureCompareParams& p);
PdeResult            run_pde             (const PdeParams& p);
VolSurfaceResult     run_vol_surface     (const VolSurfaceParams& p);

// ── ScreenerEngine ────────────────────────────────────────────────────────────

struct ScreenerParams {
    double spot       = 0.0;
    double rate       = 0.0;
    double multiplier = 1.0;                 // Deribit: 1 contract = 1 BTC / ETH
    ScreenPriceMode price_mode = ScreenPriceMode::Executable;
    bool   compute_greeks = true;            // false: keep greeks supplied with the chain
    // Reference volatility for model_value / edge:
    //   "mark" (each option's own iv) | "flat" | "surface" | "heston" | "none"
    std::string model_vol = "mark";
    double model_vol_flat = 0.0;
    std::vector<double> surface_strikes, surface_expiries, surface_ivs;
    HestonPriceParams heston;                // spot / strike / maturity / rate ignored
    std::vector<ChainOption> chain;
    ScreenStrategyToggles strategies;
    ScreenOptionFilter    option_filter;
    ScreenStrategyFilter  strategy_filter;
    ScreenRank            rank;
};

// Takes params by value: greeks are written into the chain copy.
ScreenerResult run_screener(ScreenerParams p);

}  // namespace quant::analytics
