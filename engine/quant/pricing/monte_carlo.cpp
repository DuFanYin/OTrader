#include "../kernel.h"
#include <algorithm>
#include <cmath>
#include <random>
#include <span>

namespace quant {

namespace {

inline double terminal_price(double spot, double rate, double vol, double maturity, double z) {
    return spot * std::exp((rate - 0.5 * vol * vol) * maturity + vol * std::sqrt(maturity) * z);
}

inline double call_put_payoff(double st, double strike, bool is_call) {
    return is_call ? std::max(st - strike, 0.0) : std::max(strike - st, 0.0);
}

double mc_pseudorandom(double spot, double strike, double rate, double vol,
                       double maturity, int n_paths, bool is_call,
                       double* out_stderr) {
    std::mt19937_64 rng(42);
    std::normal_distribution<double> nd;
    const double disc = std::exp(-rate * maturity);
    const double n = static_cast<double>(n_paths);
    double payoff_sum = 0.0, payoff_sq = 0.0;
    for (int i = 0; i < n_paths; ++i) {
        const double payoff = call_put_payoff(
            terminal_price(spot, rate, vol, maturity, nd(rng)), strike, is_call);
        payoff_sum += payoff;
        payoff_sq  += payoff * payoff;
    }
    const double mean = payoff_sum / n;
    if (out_stderr) {
        const double var = payoff_sq / n - mean * mean;
        *out_stderr = disc * std::sqrt(clamp_nonnegative(var) / n);
    }
    return disc * mean;
}

double mc_antithetic(double spot, double strike, double rate, double vol,
                     double maturity, int n_paths, bool is_call,
                     double* out_stderr) {
    std::mt19937_64 rng(42);
    std::normal_distribution<double> nd;
    const double disc = std::exp(-rate * maturity);
    const int half = std::max(1, n_paths / 2);
    const double n = static_cast<double>(half);
    double payoff_sum = 0.0, payoff_sq = 0.0;
    for (int i = 0; i < half; ++i) {
        const double z = nd(rng);
        const double p1 = call_put_payoff(terminal_price(spot, rate, vol, maturity,  z), strike, is_call);
        const double p2 = call_put_payoff(terminal_price(spot, rate, vol, maturity, -z), strike, is_call);
        const double avg = 0.5 * (p1 + p2);
        payoff_sum += avg;
        payoff_sq  += avg * avg;
    }
    const double mean = payoff_sum / n;
    if (out_stderr) {
        const double var = payoff_sq / n - mean * mean;
        *out_stderr = disc * std::sqrt(clamp_nonnegative(var) / n);
    }
    return disc * mean;
}

// Rational approximation for the normal quantile (Beasley-Springer-Moro).
// Accurate to ~1e-9 for p in (0,1).
static double norm_quantile(double p) {
    static const double a[] = { 2.50662823884, -18.61500062529,  41.39119773534, -25.44106049637 };
    static const double b[] = { -8.47351093090,  23.08336743743, -21.06224101826,  3.13082909833 };
    static const double c[] = { 0.3374754822726147, 0.9761690190917186, 0.1607979714918209,
                                  0.0276438810333863, 0.0038405729373609, 0.0003951896511349,
                                  0.0000321767881768, 0.0000002888167364, 0.0000003960315187 };
    const double q = p - 0.5;
    if (std::abs(q) < 0.42) {
        const double r = q * q;
        return q * (((a[3]*r + a[2])*r + a[1])*r + a[0]) /
                   ((((b[3]*r + b[2])*r + b[1])*r + b[0])*r + 1.0);
    }
    const double r0 = p < 0.5 ? p : 1.0 - p;
    const double r = std::sqrt(-std::log(std::max(r0, 1e-300)));
    double s = c[0];
    for (int i = 1; i <= 8; ++i) s = s * r + c[i];
    return p < 0.5 ? -s : s;
}

double mc_sobol(double spot, double strike, double rate, double vol,
                double maturity, int n_paths, bool is_call,
                double* out_stderr) {
    SobolEngine sobol(1);
    const double disc = std::exp(-rate * maturity);
    const double n = static_cast<double>(n_paths);
    double payoff_sum = 0.0, payoff_sq = 0.0;
    double u[1];
    for (int i = 0; i < n_paths; ++i) {
        sobol.next(std::span<double>(u, 1));
        const double uc = std::clamp(u[0], 1e-12, 1.0 - 1e-12);
        const double z = norm_quantile(uc);
        const double payoff = call_put_payoff(
            terminal_price(spot, rate, vol, maturity, z), strike, is_call);
        payoff_sum += payoff;
        payoff_sq  += payoff * payoff;
    }
    const double mean = payoff_sum / n;
    if (out_stderr) {
        const double var = payoff_sq / n - mean * mean;
        *out_stderr = disc * std::sqrt(clamp_nonnegative(var) / n);
    }
    return disc * mean;
}

}  // namespace

double mc_price(double spot, double strike, int n_paths) {
    return mc_price_full(spot, strike, 0.02, 0.2, 1.0, n_paths);
}

double mc_price_full(double spot, double strike, double rate, double vol,
                     double maturity, int n_paths) {
    return mc_price_full(spot, strike, rate, vol, maturity, n_paths, true);
}

double mc_price_full(double spot, double strike, double rate, double vol,
                     double maturity, int n_paths, bool is_call) {
    return mc_price_full(spot, strike, rate, vol, maturity, n_paths, is_call,
                         SamplerType::Pseudorandom);
}

double mc_price_full(double spot, double strike, double rate, double vol,
                     double maturity, int n_paths, bool is_call, SamplerType sampler) {
    if (spot <= 0.0 || strike <= 0.0 || n_paths <= 0) return 0.0;
    if (sampler == SamplerType::Antithetic)
        return mc_antithetic(spot, strike, rate, vol, maturity, n_paths, is_call, nullptr);
    if (sampler == SamplerType::Sobol)
        return mc_sobol(spot, strike, rate, vol, maturity, n_paths, is_call, nullptr);
    return mc_pseudorandom(spot, strike, rate, vol, maturity, n_paths, is_call, nullptr);
}

double mc_price_with_stderr(double spot, double strike, double rate, double vol,
                             double maturity, int n_paths, double* out_stderr) {
    return mc_price_with_stderr(spot, strike, rate, vol, maturity, n_paths,
                                out_stderr, true, SamplerType::Pseudorandom);
}

double mc_price_with_stderr(double spot, double strike, double rate, double vol,
                             double maturity, int n_paths, double* out_stderr, bool is_call) {
    return mc_price_with_stderr(spot, strike, rate, vol, maturity, n_paths,
                                out_stderr, is_call, SamplerType::Pseudorandom);
}

double mc_price_with_stderr(double spot, double strike, double rate, double vol,
                             double maturity, int n_paths, double* out_stderr,
                             bool is_call, SamplerType sampler) {
    if (spot <= 0.0 || strike <= 0.0 || n_paths <= 0) {
        if (out_stderr) *out_stderr = 0.0;
        return 0.0;
    }
    if (sampler == SamplerType::Antithetic)
        return mc_antithetic(spot, strike, rate, vol, maturity, n_paths, is_call, out_stderr);
    if (sampler == SamplerType::Sobol)
        return mc_sobol(spot, strike, rate, vol, maturity, n_paths, is_call, out_stderr);
    return mc_pseudorandom(spot, strike, rate, vol, maturity, n_paths, is_call, out_stderr);
}

}  // namespace quant
