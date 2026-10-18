// Selection among trials: the deflated Sharpe ratio, the probability of backtest overfitting and a
// walk-forward, on a matrix of the trials' returns by period (kernel.h).

#include "../kernel.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <bit>
#include <numeric>
#include <random>
#include <set>

namespace quant {

namespace {

constexpr double kEuler = 0.5772156649015329;

double normal_cdf(double x) { return 0.5 * std::erfc(-x / std::numbers::sqrt2); }

// Acklam's rational approximation, refined by one Halley step: |error| < 1e-12.
double normal_quantile(double p) {
    constexpr double a[] = {-3.969683028665376e+01, 2.209460984245205e+02, -2.759285104469687e+02,
                            1.383577518672690e+02, -3.066479806614716e+01, 2.506628277459239e+00};
    constexpr double b[] = {-5.447609879822406e+01, 1.615858368580409e+02, -1.556989798598866e+02,
                            6.680131188771972e+01, -1.328068155288572e+01};
    constexpr double c[] = {-7.784894002430293e-03, -3.223964580411365e-01, -2.400758277161838e+00,
                            -2.549732539343734e+00, 4.374664141464968e+00, 2.938163982698783e+00};
    constexpr double d[] = {7.784695709041462e-03, 3.224671290700398e-01, 2.445134137142996e+00,
                            3.754408661907416e+00};
    constexpr double low = 0.02425;
    double x = 0.0;
    if (p < low) {
        const double q = std::sqrt(-2 * std::log(p));
        x = (((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) /
            ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1);
    } else if (p <= 1 - low) {
        const double q = p - 0.5;
        const double r = q * q;
        x = (((((a[0] * r + a[1]) * r + a[2]) * r + a[3]) * r + a[4]) * r + a[5]) * q /
            (((((b[0] * r + b[1]) * r + b[2]) * r + b[3]) * r + b[4]) * r + 1);
    } else {
        const double q = std::sqrt(-2 * std::log(1 - p));
        x = -(((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) /
            ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1);
    }
    const double e = normal_cdf(x) - p;
    const double u = e * std::sqrt(2 * std::numbers::pi) * std::exp(x * x / 2);
    return x - u / (1 + x * u / 2);
}

// Count, sum and sum of squares of returns, NaN skipped: what a Sharpe ratio is made of.
struct Moments {
    double n = 0.0;
    double sum = 0.0;
    double squares = 0.0;
    void add(double x) {
        if (!std::isnan(x)) {
            n += 1;
            sum += x;
            squares += x * x;
        }
    }
    void add(const Moments& m) {
        n += m.n;
        sum += m.sum;
        squares += m.squares;
    }
    [[nodiscard]] double sharpe() const {
        if (n < 2) {
            return 0.0;
        }
        const double mean = sum / n;
        const double variance = (squares - sum * mean) / (n - 1);
        return variance > 0 ? mean / std::sqrt(variance) : 0.0;
    }
};

std::span<const double> row(std::span<const double> returns, int trial, int periods) {
    return returns.subspan(static_cast<std::size_t>(trial) * periods, periods);
}

// The trial with the highest Sharpe ratio over periods [first, first + count).
int best_on(std::span<const double> returns, int trials, int periods, int first, int count) {
    int best = 0;
    double best_sharpe = -INFINITY;
    for (int k = 0; k < trials; ++k) {
        Moments m;
        for (const double x : row(returns, k, periods).subspan(first, count)) {
            m.add(x);
        }
        if (m.sharpe() > best_sharpe) {
            best_sharpe = m.sharpe();
            best = k;
        }
    }
    return best;
}

}  // namespace

double period_sharpe(std::span<const double> returns) {
    Moments m;
    for (const double x : returns) {
        m.add(x);
    }
    return m.sharpe();
}

DeflatedSharpe deflated_sharpe(std::span<const double> returns, int trials, int periods, int tries) {
    DeflatedSharpe out;
    if (trials < 1 || periods < 1) {
        return out;
    }
    std::vector<double> sharpes(trials);
    for (int k = 0; k < trials; ++k) {
        sharpes[k] = period_sharpe(row(returns, k, periods));
    }
    out.best = static_cast<int>(std::ranges::max_element(sharpes) - sharpes.begin());
    out.sharpe = sharpes[out.best];

    std::vector<double> x;
    for (const double v : row(returns, out.best, periods)) {
        if (!std::isnan(v)) {
            x.push_back(v);
        }
    }
    out.periods = static_cast<int>(x.size());
    if (out.periods < 3 || tries < 1) {
        return out;
    }
    const double variance = trials > 1 ? sample_variance(sharpes) : 0.0;
    if (tries > 1) {
        const double n = tries;
        out.expected_max_sharpe =
            std::sqrt(variance) * ((1 - kEuler) * normal_quantile(1 - 1 / n) +
                                   kEuler * normal_quantile(1 - 1 / (n * std::numbers::e)));
    }
    const double mean = sample_mean(x);
    double m2 = 0.0;
    double m3 = 0.0;
    double m4 = 0.0;
    for (const double v : x) {
        const double d = v - mean;
        m2 += d * d;
        m3 += d * d * d;
        m4 += d * d * d * d;
    }
    m2 /= out.periods;
    m3 /= out.periods;
    m4 /= out.periods;
    out.skew = m2 > 0 ? m3 / std::pow(m2, 1.5) : 0.0;
    out.kurtosis = m2 > 0 ? m4 / (m2 * m2) : 3.0;
    const double denominator =
        1 - out.skew * out.sharpe + (out.kurtosis - 1) / 4 * out.sharpe * out.sharpe;
    if (denominator <= 0) {
        return out;
    }
    const double z = (out.sharpe - out.expected_max_sharpe) * std::sqrt(out.periods - 1.0) /
                     std::sqrt(denominator);
    out.dsr = normal_cdf(z);
    out.valid = true;
    return out;
}

Overfitting backtest_overfitting(std::span<const double> returns, int trials, int periods, int blocks,
                                 int max_splits, std::uint64_t seed) {
    Overfitting out;
    blocks = std::min({blocks, periods, 30}); // the in-sample blocks are a 32-bit set
    blocks -= blocks % 2;
    if (trials < 2 || blocks < 4) {
        return out;
    }
    out.blocks = blocks;
    // Each trial's moments block by block: a split's Sharpe ratios are then sums of blocks.
    std::vector<Moments> by_block(static_cast<std::size_t>(trials) * blocks);
    for (int k = 0; k < trials; ++k) {
        const auto r = row(returns, k, periods);
        for (int b = 0; b < blocks; ++b) {
            const int first = static_cast<int>(static_cast<long>(periods) * b / blocks);
            const int last = static_cast<int>(static_cast<long>(periods) * (b + 1) / blocks);
            for (int i = first; i < last; ++i) {
                by_block[static_cast<std::size_t>(k) * blocks + b].add(r[i]);
            }
        }
    }

    // The splits: a bit set of the blocks in sample, half of them.
    const int half = blocks / 2;
    std::vector<std::uint32_t> splits;
    double all = 1.0;
    for (int i = 0; i < half; ++i) {
        all = all * (blocks - i) / (i + 1);
    }
    if (all <= max_splits) {
        for (std::uint32_t mask = 0; mask < (1U << blocks); ++mask) {
            if (std::popcount(mask) == half) {
                splits.push_back(mask);
            }
        }
    } else {
        std::mt19937_64 rng(seed);
        std::set<std::uint32_t> drawn;
        std::vector<int> order(blocks);
        std::iota(order.begin(), order.end(), 0);
        while (static_cast<int>(drawn.size()) < max_splits) {
            std::ranges::shuffle(order, rng);
            std::uint32_t mask = 0;
            for (int i = 0; i < half; ++i) {
                mask |= 1U << order[i];
            }
            drawn.insert(mask);
        }
        splits.assign(drawn.begin(), drawn.end());
    }

    std::vector<double> logits(splits.size());
#pragma omp parallel for schedule(static)
    for (std::size_t s = 0; s < splits.size(); ++s) {
        int best = 0;
        double best_in = -INFINITY;
        std::vector<double> out_sharpe(trials);
        for (int k = 0; k < trials; ++k) {
            Moments in;
            Moments outside;
            for (int b = 0; b < blocks; ++b) {
                const Moments& m = by_block[static_cast<std::size_t>(k) * blocks + b];
                ((splits[s] >> b) & 1U) != 0 ? in.add(m) : outside.add(m);
            }
            out_sharpe[k] = outside.sharpe();
            if (in.sharpe() > best_in) {
                best_in = in.sharpe();
                best = k;
            }
        }
        double below = 0.0;
        double equal = 0.0;
        for (const double v : out_sharpe) {
            below += v < out_sharpe[best] ? 1 : 0;
            equal += v == out_sharpe[best] ? 1 : 0;
        }
        const double rank = below + 0.5 * (equal - 1) + 1;
        const double w = rank / (trials + 1.0);
        logits[s] = std::log(w / (1 - w));
    }
    out.splits = static_cast<int>(splits.size());
    out.pbo = static_cast<double>(std::ranges::count_if(logits, [](double l) { return l <= 0; })) /
              out.splits;
    std::ranges::sort(logits);
    out.logit_median = interp_quantile_sorted(logits, 0.5);
    out.valid = true;
    return out;
}

WalkForward walk_forward(std::span<const double> returns, int trials, int periods, int train,
                         int test) {
    WalkForward out;
    if (trials < 1 || train < 2 || test < 1 || train + test > periods) {
        return out;
    }
    Moments strung;
    for (int first = 0; first + train + test <= periods; first += test) {
        WalkForwardWindow w;
        w.train_first = first;
        w.test_first = first + train;
        w.best = best_on(returns, trials, periods, first, train);
        Moments m;
        for (const double x : row(returns, w.best, periods).subspan(w.test_first, test)) {
            m.add(x);
            strung.add(x);
        }
        w.test_net = m.sum;
        w.test_sharpe = m.sharpe();
        out.positive += w.test_net > 0 ? 1 : 0;
        out.windows.push_back(w);
    }
    out.net = strung.sum;
    out.sharpe = strung.sharpe();
    return out;
}

}  // namespace quant
