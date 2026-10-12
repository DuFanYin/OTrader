#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <random>
#include <array>
#include <span>
#include <vector>

#include "quant/screener/screener.h"

namespace quant {

// ── Optimizer ─────────────────────────────────────────────────────────────────

struct OptResult {
    std::vector<double> x;
    double fval      = 1e30;
    int    iterations = 0;
    bool   converged  = false;
};

OptResult nelder_mead(
    std::function<double(std::span<const double>)> objective,
    std::span<const double> x0,
    std::span<const double> lower,
    std::span<const double> upper,
    double tol      = 1e-6,
    int    max_iter = 1000
);

// ── Numerics ──────────────────────────────────────────────────────────────────

double clamp_positive   (double x, double eps = 1e-8);
double clamp_nonnegative(double x);
double norm_cdf(double x);
double norm_pdf(double x);

// ── Stats ─────────────────────────────────────────────────────────────────────

double sample_mean              (const std::vector<double>& xs);
double sample_variance          (const std::vector<double>& xs);
double interp_quantile_sorted   (const std::vector<double>& sorted_xs, double q);

double stats_mgf(double mu, double sigma, double theta);
// Sample moments of `sample_size` draws from N(mu, sigma²): E[e^{θX}], mean, variance.
void   stats_normal(double mu, double sigma, double theta, int sample_size,
                    double* out_mgf, double* out_mean, double* out_variance);

// Itô checks on f(W) (0: e^{θW − θ²t/2}, 1: W² − t, 2: W³).
struct ItoCheck {
    double value     = 0.0;   // E[f(W_t)] by Monte Carlo
    double target    = 0.0;   // its exact value
    double std_error = 0.0;   // of value
    double residual  = 0.0;   // RMS gap of the discretised Itô formula over the paths, relative to f(W_t) − f(0)
    int    paths     = 0;     // paths of n_steps the residual was measured on
};
ItoCheck ito_check(int function_code, double theta, double t, int n_steps);

// ── MC Sampler ────────────────────────────────────────────────────────────────

enum class SamplerType { Pseudorandom = 0, Antithetic = 1, Sobol = 2 };

class SobolEngine {
public:
    explicit SobolEngine(int dimension);
    void next(std::span<double> out_unit);
    void skip(int n);
private:
    int dim_;
    unsigned counter_;
    std::vector<std::vector<uint32_t>> V_;
    std::vector<uint32_t> X_;
};

// ── Simulation ────────────────────────────────────────────────────────────────

enum class ModelType { GBM = 0, Vasicek = 1 };

struct ModelParams {
    double mu    = 0.0;
    double sigma = 0.2;
    double kappa = 1.2;
    double theta = 0.03;
};

class StochasticModel {
public:
    virtual ~StochasticModel() = default;
    virtual double drift    (double x, double t) const = 0;
    virtual double diffusion(double x, double t) const = 0;
    virtual double step     (double x, double t, double dt, double z) const = 0;
};

class GbmModel final : public StochasticModel {
public:
    explicit GbmModel(const ModelParams& p);
    double drift    (double x, double t) const override;
    double diffusion(double x, double t) const override;
    double step     (double x, double t, double dt, double z) const override;
private:
    ModelParams p_;
};

class VasicekModel final : public StochasticModel {
public:
    explicit VasicekModel(const ModelParams& p);
    double drift    (double x, double t) const override;
    double diffusion(double x, double t) const override;
    double step     (double x, double t, double dt, double z) const override;
private:
    ModelParams p_;
};

std::unique_ptr<StochasticModel> build_model(ModelType type, const ModelParams& params);
double simulate_terminal_price_gbm(double spot, double rate, double vol, double maturity,
                                   std::mt19937_64& rng);

int simulation_path    (int model_code, int n_steps, double dt,
                        double sigma, double kappa, double theta, double x0,
                        std::span<double> out_values);
int measure_density_path(double mu, double r, double sigma, double t,
                         int n_steps, std::span<double> out_values);
int measure_compare    (double mu, double r, double sigma, double t,
                        int n_steps, int n_paths, double x0,
                        std::span<double> out_stats_10, std::span<double> out_preview, int preview_len);

// ── Pricing ───────────────────────────────────────────────────────────────────

// Black on the forward, discounted at `rate`: the trading engine's Greeks and implied vol.
/** Per-unit Greeks; theta per day, vega per 1% vol move (as py_vollib). */
struct BlackGreeks {
    double delta = 0.0;
    double gamma = 0.0;
    double theta = 0.0;
    double vega = 0.0;
};
/** Greeks of an option priced as Black on `forward`, discounted at `rate`. Delta and gamma are to
 * `spot`, the forward moving with it (in proportion); theta holds the forward. */
BlackGreeks black_greeks(bool is_call, double spot, double forward, double strike, double maturity,
                         double rate, double vol);
/** Black on `forward`, discounted at `rate`: the option's price; 0 for a non-positive input. */
double black_price(bool is_call, double forward, double strike, double maturity, double rate,
                   double vol);
/** The vol at which Black on `forward`, discounted at `rate`, gives `price` (LetsBeRational, to
 * machine precision); 0 when there is none, at most 5. */
double black_implied_vol(double price, double forward, double strike, double maturity, double rate,
                         bool is_call);

double bs_closed_form_price (double spot, double strike, double rate, double vol, double maturity);
double bs_closed_form_price (double spot, double strike, double rate, double vol, double maturity, bool is_call);
double bs_delta             (double spot, double strike, double rate, double vol, double maturity);
double digital_call_bs_price(double spot, double strike, double rate, double vol,
                              double maturity, double dividend_yield);

double mc_price            (double spot, double strike, int n_paths);
double mc_price_full       (double spot, double strike, double rate, double vol,
                            double maturity, int n_paths);
double mc_price_full       (double spot, double strike, double rate, double vol,
                            double maturity, int n_paths, bool is_call);
double mc_price_full       (double spot, double strike, double rate, double vol,
                            double maturity, int n_paths, bool is_call, SamplerType sampler);
double mc_price_with_stderr(double spot, double strike, double rate, double vol,
                            double maturity, int n_paths, double* out_stderr);
double mc_price_with_stderr(double spot, double strike, double rate, double vol,
                            double maturity, int n_paths, double* out_stderr, bool is_call);
double mc_price_with_stderr(double spot, double strike, double rate, double vol,
                            double maturity, int n_paths, double* out_stderr, bool is_call,
                            SamplerType sampler);

double binomial_price         (double spot, double strike, double rate, double vol,
                               double maturity, int steps);
double binomial_price         (double spot, double strike, double rate, double vol,
                               double maturity, int steps, bool is_call);
double binomial_american_price(double spot, double strike, double rate, double vol,
                               double maturity, int steps, double dividend_yield = 0.0);

void pricing_greeks     (double spot, double strike, double rate, double vol, double maturity,
                         double dividend_yield,
                         double* out_delta_bs, double* out_gamma_bs,
                         double* out_theta_bs, double* out_vega_bs, double* out_rho_bs);
void greek_surface_grid (double strike, double rate, double vol, double dividend_yield,
                         std::span<const double> spots, std::span<const double> maturities,
                         int greek_code, std::span<double> out_grid);
void pricing_error_decomp(double mc, double bs, double binomial,
                          double* out_mc_minus_bs, double* out_binomial_minus_bs);
void scenario_bs5       (double spot, double strike, double rate, double vol,
                         double maturity, double dividend_yield, bool is_call, double* out_prices_5);
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

double bs_implied_vol   (double market_price, double spot, double strike,
                         double rate, double maturity, double dividend_yield);
void   implied_vol_batch(std::span<const double>  market_prices,
                         std::span<const double>  strikes,
                         std::span<const double>  expiries,
                         double spot, double rate, double dividend_yield,
                         std::span<double>        out_ivs,
                         std::span<uint8_t>       out_converged);

double heston_call_price(double spot, double strike, double rate, double maturity,
                         double v0, double kappa, double theta, double xi, double rho,
                         int quad_points = 64);
void   heston_price_batch(double spot, double rate,
                          std::span<const double> strikes,
                          std::span<const double> maturities,
                          double v0, double kappa, double theta, double xi, double rho,
                          std::span<double> out_prices);

struct HestonParams {
    double v0 = 0.04, kappa = 1.5, theta = 0.04, xi = 0.5, rho = -0.7;
};

struct HestonFit {
    HestonParams params;
    double rmse       = 0.0;   // of the call prices, at params
    int    iterations = 0;
    bool   converged  = false;
};

// Heston fitted to call prices: least squares by Nelder-Mead from `init`, within fixed bounds.
HestonFit heston_calibrate(double spot, double rate,
                           std::span<const double> strikes,
                           std::span<const double> maturities,
                           std::span<const double> call_prices,
                           const HestonParams& init, int max_iter = 500);

double pde_price        (double spot, double strike, double rate, double vol,
                         double maturity, double dividend_yield,
                         int s_steps, int t_steps, int method);

constexpr int kPdeMaxExplicitSteps = 2'000'000;

struct PdeSpec {
    double spot = 0.0, strike = 0.0, rate = 0.0, dividend_yield = 0.0, vol = 0.0, maturity = 0.0;
    int    s_steps = 200, t_steps = 200;
    int    method  = 0;           // 0 Crank-Nicolson, 1 implicit, 2 explicit
    bool   is_call = true;
    bool   american = false;      // implicit / CN: PSOR; explicit: max with intrinsic
    double s_max_mult = 4.0;      // S grid upper bound = s_max_mult * max(K, S0)
    double psor_omega = 1.2;
    double psor_tol   = 1e-8;
    int    psor_max_iter = 20'000;
};

struct PdeOutcome {
    double    price = 0.0;
    int       t_steps_used = 0;   // explicit scheme refines the time grid to stay stable
    bool      refined = false;
    long long psor_iterations = 0;
};

PdeOutcome pde_solve(const PdeSpec& spec);

// Lattices with dividend yield; American = early exercise at every node.
double trinomial_price   (double spot, double strike, double rate, double dividend_yield, double vol,
                          double maturity, int steps, bool is_call, bool american);
double binomial_crr_price(double spot, double strike, double rate, double dividend_yield, double vol,
                          double maturity, int steps, bool is_call, bool american);

// Longstaff-Schwartz American option, basis {1, S/K, (S/K)^2} on in-the-money paths.
// Deterministic for a given seed regardless of thread count.
struct LsmResult {
    double price  = 0.0;
    double std_error = 0.0;
    int    n_paths = 0;
};
LsmResult lsm_american_price(double spot, double strike, double rate, double dividend_yield, double vol,
                             double maturity, int n_paths, int n_steps, bool is_call,
                             uint64_t seed = 42, bool antithetic = true);
double vol_surface_interp(std::span<const double> strikes,
                          std::span<const double> expiries,
                          std::span<const double> ivs,
                          double spot,
                          double target_strike, double target_expiry);

// ── Hedging ───────────────────────────────────────────────────────────────────

// One leg of a position to hedge: `quantity` contracts held (negative: sold), priced with Black-Scholes at `vol` on a spot that
// carries `yield` (the dividend yield, or r - ln(F/S)/T for a leg priced off its own forward F).
struct HedgeLeg {
    bool   is_call  = true;
    double strike   = 0.0;
    double quantity = 1.0;
    double vol      = 0.2;
    double maturity = 1.0;
    double yield    = 0.0;
};

// The hedging strategies hedge_position runs, in its order.
inline constexpr std::array<const char*, 4> kHedgeStrategies = {
    "discrete_delta", "with_transaction_cost", "threshold_rebalance", "vol_mismatch"};

// What one hedging strategy made on each path: P&L (the hedger's), shares traded, fees paid.
struct HedgeStrategy {
    const char* name = "";
    std::vector<double> pnl, turnover, cost;
};

// The position taken at its Black-Scholes value and delta-hedged with the underlying, on n_paths paths of a GBM (drift
// rate - dividend_yield, vol path_vol) over the longest leg, rebalanced n_rebalances times; each leg settles in cash at
// its expiry. Four strategies on the same paths: discrete delta; with transaction costs; rebalancing only when delta has
// moved by more than the threshold (with costs); deltas from vol × mismatch (with costs). P&L = payoffs + hedge - the
// position's cost, so costs lower it. Deterministic whatever the number of threads.
std::array<HedgeStrategy, 4> hedge_position(double spot, double rate, double dividend_yield, double path_vol,
                                            std::span<const HedgeLeg> legs, int n_rebalances, int n_paths,
                                            double transaction_cost_bps, double rebalance_threshold,
                                            double vol_mismatch_mult);

// A strategy's P&L over its paths: moments and quantiles, mean shares traded and fees, VaR95 and ES95 as positive losses.
struct PnlStats {
    double mean = 0.0, std = 0.0, q05 = 0.0, q50 = 0.0, q95 = 0.0;
    double turnover = 0.0, cost = 0.0, var95 = 0.0, es95 = 0.0;
};
PnlStats hedge_stats(const HedgeStrategy& run);
void pnl_histogram(const std::vector<double>& pnl, int n_bins, std::span<double> out_edges, std::span<double> out_counts);

}  // namespace quant
