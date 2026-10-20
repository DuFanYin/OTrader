#pragma once
// The research tools' tasks on quant, each a JSON request answered in JSON: one function a task,
// written in the workflow file of its kind (pricing, scenario, hedging, validation, utility,
// calibration, screener); c_api.cpp maps a task's name to it.

#include <span>
#include <string>

namespace quant {

// Kernels only the tools use, beside their workflows (pricing_engine, scenario_engine,
// utility_engine).
void greek_surface_grid (double strike, double rate, double vol, double dividend_yield,
                         std::span<const double> spots, std::span<const double> maturities,
                         int greek_code, std::span<double> out_grid);
// Calls: Monte Carlo, Black-Scholes and a European CRR tree of 250 steps a year (at least 10).
void pricing_batch      (std::span<const double> spots,
                         std::span<const double> strikes,
                         std::span<const double> rates,
                         std::span<const double> vols,
                         std::span<const double> maturities,
                         std::span<const int> n_paths_arr,
                         std::span<const double> div_yields,
                         std::span<double> out_mc,
                         std::span<double> out_bs,
                         std::span<double> out_binomial);
void scenario_bs5       (double spot, double strike, double rate, double vol,
                         double maturity, double dividend_yield, bool is_call,
                         double* out_prices_5);
int simulation_path    (int model_code, int n_steps, double dt,
                        double sigma, double kappa, double theta, double x0,
                        std::span<double> out_values);
int measure_density_path(double mu, double r, double sigma, double t,
                         int n_steps, std::span<double> out_values);
int measure_compare    (double mu, double r, double sigma, double t,
                        int n_steps, int n_paths, double x0,
                        std::span<double> out_stats_10, std::span<double> out_preview,
                        int preview_len);
double vol_surface_interp(std::span<const double> strikes,
                          std::span<const double> expiries,
                          std::span<const double> ivs,
                          double spot,
                          double target_strike, double target_expiry);

}  // namespace quant

namespace quant::analytics {

// The version of every response's shape. Bump it when a response changes; the server checks it on load
// (quant_contract_version) and reports a stale build instead of misreading it.
inline constexpr const char* kContractVersion = "4.1";

// Each answers in one shape: contract_version, status ("ok" | "error"), result_summary,
// result_details, runtime_ms and error ({code, message}; null when ok).

std::string run_pricing_json(const std::string& request_json, double t0_ms);
std::string run_pricing_batch_json(const std::string& request_json, double t0_ms);
std::string run_pricing_batch_grid_json(const std::string& request_json, double t0_ms);
std::string run_hedging_json(const std::string& request_json, double t0_ms);
std::string run_scenario_json(const std::string& request_json, double t0_ms);
std::string run_validation_json(const std::string& request_json, double t0_ms);
std::string run_simulation_json(const std::string& request_json, double t0_ms);
std::string run_stats_json(const std::string& request_json, double t0_ms);
std::string run_ito_json(const std::string& request_json, double t0_ms);
std::string run_optimize_json(const std::string& request_json, double t0_ms);
std::string run_measure_density_json(const std::string& request_json, double t0_ms);
std::string run_measure_compare_json(const std::string& request_json, double t0_ms);
std::string run_pde_json(const std::string& request_json, double t0_ms);
std::string run_vol_surface_json(const std::string& request_json, double t0_ms);
std::string run_greek_surface_json(const std::string& request_json, double t0_ms);
std::string run_lattice_json(const std::string& request_json, double t0_ms);
std::string run_multi_leg_json(const std::string& request_json, double t0_ms);
std::string run_implied_vol_json(const std::string& request_json, double t0_ms);
std::string run_implied_vol_batch_json(const std::string& request_json, double t0_ms);
std::string run_black_batch_json(const std::string& request_json, double t0_ms);
std::string run_synthetic_quotes_json(const std::string& request_json, double t0_ms);
std::string run_heston_calibrate_json(const std::string& request_json, double t0_ms);
std::string run_heston_price_json(const std::string& request_json, double t0_ms);
std::string run_screener_json(const std::string& request_json, double t0_ms);

// Tasks on a matrix of doubles passed beside the request (quant_run_matrix), not in it.
std::string run_selection_json(const std::string& request_json, std::span<const double> values, double t0_ms);

}  // namespace quant::analytics
