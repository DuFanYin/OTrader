#include "../kernel.h"

#include <algorithm>
#include <cmath>
#include <random>

namespace quant {

double sample_mean(const std::vector<double>& xs) {
    if (xs.empty()) return 0.0;
    double s = 0.0;
    for (double x : xs) s += x;
    return s / static_cast<double>(xs.size());
}

double sample_variance(const std::vector<double>& xs) {
    if (xs.size() < 2) return 0.0;
    const double m = sample_mean(xs);
    double acc = 0.0;
    for (double x : xs) { const double d = x - m; acc += d * d; }
    return acc / static_cast<double>(xs.size() - 1);
}

double interp_quantile_sorted(const std::vector<double>& sorted_xs, double q) {
    if (sorted_xs.empty()) return 0.0;
    const double qc = std::min(1.0, std::max(0.0, q));
    const int    n  = static_cast<int>(sorted_xs.size());
    const double pos = qc * (n - 1);
    const int    lo  = static_cast<int>(pos);
    const int    hi  = std::min(lo + 1, n - 1);
    return sorted_xs[lo] * (1.0 - (pos - lo)) + sorted_xs[hi] * (pos - lo);
}

void stats_normal(
    double mu, double sigma, double theta, int sample_size,
    double* out_mgf, double* out_mean, double* out_variance
) {
    const double s = clamp_positive(sigma);
    const int    n = std::max(sample_size, 2);
    std::mt19937_64 rng(7);
    std::normal_distribution<double> z(mu, s);
    double mean = 0.0, m2 = 0.0, mgf = 0.0;
    for (int k = 0; k < n; ++k) {
        const double x = z(rng), d = x - mean;
        mean += d / (k + 1);
        m2   += d * (x - mean);
        mgf  += (std::exp(theta * x) - mgf) / (k + 1);
    }
    if (out_mgf)      *out_mgf      = mgf;
    if (out_mean)     *out_mean     = mean;
    if (out_variance) *out_variance = m2 / (n - 1);
}

double stats_mgf(double mu, double sigma, double theta) {
    const double s = clamp_positive(sigma);
    return std::exp(mu * theta + 0.5 * s * s * theta * theta);
}

}  // namespace quant
