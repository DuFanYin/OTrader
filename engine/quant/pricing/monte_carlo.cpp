#include "../kernel.h"
#include <algorithm>
#include <cmath>
#include <random>
#include <span>

namespace quant {

namespace {

inline double terminal_price(double spot, double carry, double vol, double maturity, double z) {
    return spot * std::exp((carry - 0.5 * vol * vol) * maturity + vol * std::sqrt(maturity) * z);
}

inline double discounted_mean(double payoff_sum, double payoff_sq, double n, double disc,
                              double* out_stderr) {
    const double mean = payoff_sum / n;
    if (out_stderr) {
        const double var = payoff_sq / n - mean * mean;
        *out_stderr = disc * std::sqrt(clamp_nonnegative(var) / n);
    }
    return disc * mean;
}

// `carry`: the drift of the spot (rate - dividend yield); `disc`: the discount to today.
double mc_pseudorandom(double spot, double strike, double carry, double disc, double vol,
                       double maturity, int n_paths, bool is_call,
                       double* out_stderr) {
    std::mt19937_64 rng(42);
    std::normal_distribution<double> nd;
    const double n = static_cast<double>(n_paths);
    double payoff_sum = 0.0, payoff_sq = 0.0;
    for (int i = 0; i < n_paths; ++i) {
        const double value =
            payoff(terminal_price(spot, carry, vol, maturity, nd(rng)), strike, is_call);
        payoff_sum += value;
        payoff_sq  += value * value;
    }
    return discounted_mean(payoff_sum, payoff_sq, n, disc, out_stderr);
}

double mc_antithetic(double spot, double strike, double carry, double disc, double vol,
                     double maturity, int n_paths, bool is_call,
                     double* out_stderr) {
    std::mt19937_64 rng(42);
    std::normal_distribution<double> nd;
    const int half = std::max(1, n_paths / 2);
    const double n = static_cast<double>(half);
    double payoff_sum = 0.0, payoff_sq = 0.0;
    for (int i = 0; i < half; ++i) {
        const double z = nd(rng);
        const double p1 =
            payoff(terminal_price(spot, carry, vol, maturity, z), strike, is_call);
        const double p2 =
            payoff(terminal_price(spot, carry, vol, maturity, -z), strike, is_call);
        const double avg = 0.5 * (p1 + p2);
        payoff_sum += avg;
        payoff_sq  += avg * avg;
    }
    return discounted_mean(payoff_sum, payoff_sq, n, disc, out_stderr);
}

double mc_sobol(double spot, double strike, double carry, double disc, double vol,
                double maturity, int n_paths, bool is_call,
                double* out_stderr) {
    SobolEngine sobol(1);
    const double n = static_cast<double>(n_paths);
    double payoff_sum = 0.0, payoff_sq = 0.0;
    double u[1];
    for (int i = 0; i < n_paths; ++i) {
        sobol.next(std::span<double>(u, 1));
        const double uc = std::clamp(u[0], 1e-12, 1.0 - 1e-12);
        const double z = normal_quantile(uc);
        const double value = payoff(terminal_price(spot, carry, vol, maturity, z), strike, is_call);
        payoff_sum += value;
        payoff_sq  += value * value;
    }
    return discounted_mean(payoff_sum, payoff_sq, n, disc, out_stderr);
}

}  // namespace

double mc_price(double spot, double strike, double rate, double dividend_yield, double vol,
                double maturity, int n_paths, bool is_call, SamplerType sampler,
                double* out_stderr) {
    if (spot <= 0.0 || strike <= 0.0 || n_paths <= 0) {
        if (out_stderr) *out_stderr = 0.0;
        return 0.0;
    }
    const double carry = rate - dividend_yield;
    const double disc = std::exp(-rate * maturity);
    if (sampler == SamplerType::Antithetic)
        return mc_antithetic(spot, strike, carry, disc, vol, maturity, n_paths, is_call,
                             out_stderr);
    if (sampler == SamplerType::Sobol)
        return mc_sobol(spot, strike, carry, disc, vol, maturity, n_paths, is_call, out_stderr);
    return mc_pseudorandom(spot, strike, carry, disc, vol, maturity, n_paths, is_call, out_stderr);
}

}  // namespace quant
