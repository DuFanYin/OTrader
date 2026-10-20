#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <random>
#include <array>
#include <span>
#include <vector>

#include "quant/engine_api.h"

namespace quant {

// ── Optimizer ─────────────────────────────────────────────────────────────────

// Bayesian optimisation's next `batch` points in the box [lower, upper] (integer[d] != 0: whole
// numbers), to maximise: a Gaussian process fitted to the points tried and their values (NaN: a try
// that failed, taken as the worst) and expected improvement, the batch by the constant liar; fewer
// tries than 2 x dims + 1, a Latin hypercube instead. No point tried is proposed again; fewer than
// `batch` when the box has no more.
std::vector<std::vector<double>> bayes_next(std::span<const double> lower,
                                            std::span<const double> upper,
                                            std::span<const char> integer,
                                            const std::vector<std::vector<double>>& tried,
                                            std::span<const double> values, int batch,
                                            std::uint64_t seed);

// ── Numerics ──────────────────────────────────────────────────────────────────

double clamp_positive   (double x, double eps = 1e-8);
double clamp_nonnegative(double x);
double norm_cdf(double x);
double norm_pdf(double x);
/** The standard normal quantile (Acklam, one Halley step: |error| < 1e-12). */
double normal_quantile  (double p);
/** A call's or a put's value at expiry. */
inline double payoff(double s, double k, bool is_call) {
    return is_call ? std::max(s - k, 0.0) : std::max(k - s, 0.0);
}

// ── Stats ─────────────────────────────────────────────────────────────────────

double sample_mean              (const std::vector<double>& xs);
double sample_variance          (const std::vector<double>& xs);
double interp_quantile_sorted   (const std::vector<double>& sorted_xs, double q);

double stats_mgf(double mu, double sigma, double theta);
// Sample moments of `sample_size` draws from N(mu, sigma²): E[e^{θX}], mean, variance.
void   stats_normal(double mu, double sigma, double theta, int sample_size,
                    double* out_mgf, double* out_mean, double* out_variance);

// Selection among trials: `returns` row-major, one row per trial and one column per period (a day), NaN
// where a trial has no return. A period's Sharpe ratio is mean / sample standard deviation, NaN skipped
// (0 when it does not vary or has fewer than two).
double period_sharpe(std::span<const double> returns);

// The best trial (by period Sharpe ratio) against the best of `tries` tries with no skill (Bailey and
// López de Prado, 2014): dsr is the probability its Sharpe ratio is above expected_max_sharpe, given the
// spread of all the trials' ratios and the best one's skew and kurtosis. valid is false with fewer than
// three periods or a distribution too skewed for the test.
struct DeflatedSharpe {
    int    best = -1;
    double sharpe = 0.0;
    double expected_max_sharpe = 0.0;
    double skew = 0.0;
    double kurtosis = 3.0;
    int    periods = 0;
    double dsr = 0.0;
    bool   valid = false;
};
DeflatedSharpe deflated_sharpe(std::span<const double> returns, int trials, int periods, int tries);

// The probability of backtest overfitting (Bailey, Borwein, López de Prado and Zhu, 2017) by
// combinatorially symmetric cross-validation: the periods cut into `blocks` (even) blocks; for each way to
// take half of them in sample (all, or `max_splits` drawn with `seed`), the trial best in sample is ranked
// out of sample; pbo is how often it lands in the bottom half. valid is false with one trial or fewer
// than four blocks' worth of periods.
struct Overfitting {
    double pbo = 0.0;
    int    splits = 0;
    int    blocks = 0;
    double logit_median = 0.0;
    bool   valid = false;
};
Overfitting backtest_overfitting(std::span<const double> returns, int trials, int periods, int blocks,
                                 int max_splits, std::uint64_t seed);

// Walk-forward: the trial best on `train` periods traded on the `test` after them, stepping by `test`.
struct WalkForwardWindow {
    int    train_first = 0;
    int    test_first = 0;
    int    best = -1;
    double test_net = 0.0;
    double test_sharpe = 0.0;
};
struct WalkForward {
    std::vector<WalkForwardWindow> windows;
    double net = 0.0;      // the test periods strung together
    double sharpe = 0.0;   // of them, per period
    int    positive = 0;   // windows with a positive test net
};
WalkForward walk_forward(std::span<const double> returns, int trials, int periods, int train, int test);

// Itô checks on f(W) (0: e^{θW − θ²t/2}, 1: W² − t, 2: W³).
struct ItoCheck {
    double value     = 0.0;   // E[f(W_t)] by Monte Carlo
    double target    = 0.0;   // its exact value
    double std_error = 0.0;   // of value
    // RMS gap of the discretised Itô formula over the paths, relative to f(W_t) − f(0).
    double residual  = 0.0;
    int    paths     = 0;     // paths of n_steps the residual was measured on
};
ItoCheck ito_check(int function_code, double theta, double t, int n_steps);

// ── MC Sampler ────────────────────────────────────────────────────────────────

enum class SamplerType { Pseudorandom = 0, Antithetic = 1, Sobol = 2 };

class SobolEngine {
public:
    explicit SobolEngine(int dimension);
    void next(std::span<double> out_unit);
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


// ── Pricing ───────────────────────────────────────────────────────────────────

// Black itself is engine_api.h's; the European closed forms here are Black on the forward too.
/** A cash-or-nothing call (1 at expiry above `strike`) under Black on `forward`, discounted at
 * `rate`. */
double black_digital_call(double forward, double strike, double maturity, double rate, double vol);

/** Black-Scholes Greeks of a call or put on a spot paying `dividend_yield`, from black_greeks on
 * its forward with the spot held: theta per year, vega per 1.00 of vol, rho at a fixed spot. */
void pricing_greeks(bool is_call, double spot, double strike, double rate, double dividend_yield,
                    double vol, double maturity, double* out_delta, double* out_gamma,
                    double* out_theta, double* out_vega, double* out_rho);

/** A European option by Monte Carlo on GBM terminal prices (drift rate - dividend_yield),
 * discounted at rate; seed 42. */
double mc_price(double spot, double strike, double rate, double dividend_yield, double vol,
                double maturity, int n_paths, bool is_call,
                SamplerType sampler = SamplerType::Pseudorandom, double* out_stderr = nullptr);

/** The vol at which Black-Scholes on a spot paying `dividend_yield` gives the call price
 * `market_price`; 0 when there is none in [1e-4, 5), as black_implied_vol. */
double bs_implied_vol   (double market_price, double spot, double strike,
                         double rate, double maturity, double dividend_yield);
void   implied_vol_batch(std::span<const double>  market_prices,
                         std::span<const double>  strikes,
                         std::span<const double>  expiries,
                         double spot, double rate, double dividend_yield,
                         std::span<double>        out_ivs,
                         std::span<uint8_t>       out_converged);

struct HestonParams {
    double v0 = 0.04, kappa = 1.5, theta = 0.04, xi = 0.5, rho = -0.7;
};

double heston_call_price(double spot, double strike, double rate, double maturity,
                         const HestonParams& h);

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

// Lattices with dividend yield; American = early exercise at every node. NaN for a non-positive
// spot, strike or vol or a tree that cannot hold the drift; the payoff at maturity 0.
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

// ── Hedging ───────────────────────────────────────────────────────────────────

// One leg of a position to hedge: `quantity` contracts held (negative: sold), priced with
// Black-Scholes at `vol` on a spot that carries `yield` (the dividend yield, or r - ln(F/S)/T for a
// leg priced off its own forward F).
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

// The position taken at its Black-Scholes value and delta-hedged with the underlying, on n_paths
// GBM paths (drift rate - dividend_yield, vol path_vol) over the longest leg, rebalanced
// n_rebalances times; each leg settles in cash at its expiry. Four strategies on the same paths:
// discrete delta; with costs; rebalance only past the delta threshold (with costs); deltas from
// vol x mismatch (with costs). P&L = payoffs + hedge - position cost. Deterministic for any
// thread count.
std::array<HedgeStrategy, 4> hedge_position(double spot, double rate, double dividend_yield, double path_vol,
                                            std::span<const HedgeLeg> legs, int n_rebalances, int n_paths,
                                            double transaction_cost_bps, double rebalance_threshold,
                                            double vol_mismatch_mult);

// A strategy's P&L over its paths: moments, quantiles, mean shares traded and fees, VaR95 and ES95
// as positive losses.
struct PnlStats {
    double mean = 0.0, std = 0.0, q05 = 0.0, q50 = 0.0, q95 = 0.0;
    double turnover = 0.0, cost = 0.0, var95 = 0.0, es95 = 0.0;
};
PnlStats hedge_stats(const HedgeStrategy& run);
void pnl_histogram(const std::vector<double>& pnl, int n_bins, std::span<double> out_edges, std::span<double> out_counts);

}  // namespace quant
