#include "quant/kernel.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <random>
#include <span>
#include <utility>
#include <vector>

namespace quant {

namespace {

using Point = std::vector<double>;

/** A Gaussian process over the unit box: squared-exponential kernel of one length scale, the values
 * standardised. */
class Process {
  public:
    Process(const std::vector<Point>& x, const std::vector<double>& y, double length, double noise)
        : x_(x), length_(length) {
        const std::size_t n = x.size();
        mean_ = 0.0;
        for (const double v : y) {
            mean_ += v;
        }
        mean_ /= static_cast<double>(n);
        double var = 0.0;
        for (const double v : y) {
            var += (v - mean_) * (v - mean_);
        }
        scale_ = n > 1 && var > 0.0 ? std::sqrt(var / static_cast<double>(n - 1)) : 1.0;
        chol_.assign(n * n, 0.0);
        for (std::size_t i = 0; i < n; ++i) {
            for (std::size_t j = 0; j <= i; ++j) {
                chol_[i * n + j] = kernel(x[i], x[j]) + (i == j ? noise : 0.0);
            }
        }
        ok_ = cholesky(n);
        alpha_.resize(n);
        for (std::size_t i = 0; i < n; ++i) {
            alpha_[i] = (y[i] - mean_) / scale_;
        }
        solve_lower(alpha_);
        fit_ = 0.0; // the log marginal likelihood, but its constant
        for (std::size_t i = 0; i < n; ++i) {
            fit_ -= 0.5 * alpha_[i] * alpha_[i] + std::log(chol_[i * n + i]);
        }
        solve_upper(alpha_);
    }

    [[nodiscard]] bool ok() const { return ok_; }
    [[nodiscard]] double fit() const { return fit_; }

    /** The mean and standard deviation at u, in the values' units. */
    [[nodiscard]] std::pair<double, double> predict(const Point& u) const {
        const std::size_t n = x_.size();
        std::vector<double> k(n);
        double mean = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            k[i] = kernel(u, x_[i]);
            mean += k[i] * alpha_[i];
        }
        solve_lower(k);
        double var = 1.0;
        for (const double v : k) {
            var -= v * v;
        }
        return {mean_ + scale_ * mean, scale_ * std::sqrt(std::max(var, 1e-12))};
    }

  private:
    [[nodiscard]] double kernel(const Point& a, const Point& b) const {
        double d2 = 0.0;
        for (std::size_t i = 0; i < a.size(); ++i) {
            d2 += (a[i] - b[i]) * (a[i] - b[i]);
        }
        return std::exp(-0.5 * d2 / (length_ * length_));
    }

    bool cholesky(std::size_t n) {
        for (std::size_t j = 0; j < n; ++j) {
            double d = chol_[j * n + j];
            for (std::size_t k = 0; k < j; ++k) {
                d -= chol_[j * n + k] * chol_[j * n + k];
            }
            if (d <= 0.0) {
                return false;
            }
            chol_[j * n + j] = std::sqrt(d);
            for (std::size_t i = j + 1; i < n; ++i) {
                double s = chol_[i * n + j];
                for (std::size_t k = 0; k < j; ++k) {
                    s -= chol_[i * n + k] * chol_[j * n + k];
                }
                chol_[i * n + j] = s / chol_[j * n + j];
            }
        }
        return true;
    }

    void solve_lower(std::vector<double>& b) const {
        const std::size_t n = x_.size();
        for (std::size_t i = 0; i < n; ++i) {
            for (std::size_t k = 0; k < i; ++k) {
                b[i] -= chol_[i * n + k] * b[k];
            }
            b[i] /= chol_[i * n + i];
        }
    }

    void solve_upper(std::vector<double>& b) const {
        const std::size_t n = x_.size();
        for (std::size_t i = n; i-- > 0;) {
            for (std::size_t k = i + 1; k < n; ++k) {
                b[i] -= chol_[k * n + i] * b[k];
            }
            b[i] /= chol_[i * n + i];
        }
    }

    const std::vector<Point>& x_;
    double length_;
    double mean_ = 0.0;
    double scale_ = 1.0;
    std::vector<double> chol_; // lower triangle, row-major
    std::vector<double> alpha_;
    double fit_ = 0.0;
    bool ok_ = false;
};

/** The process whose length scale and noise fit the values best, from a small grid. */
auto fitted(const std::vector<Point>& x, const std::vector<double>& y) -> Process {
    std::optional<Process> best;
    for (const double length : {0.08, 0.15, 0.25, 0.4, 0.7, 1.2}) {
        for (const double noise : {1e-6, 1e-3, 1e-2, 0.1, 0.3}) {
            Process p(x, y, length, noise);
            if (p.ok() && (!best || p.fit() > best->fit())) {
                best.emplace(std::move(p));
            }
        }
    }
    return best ? std::move(*best) : Process(x, y, 0.4, 1.0);
}

auto expected_improvement(double mean, double sd, double best) -> double {
    const double gain = mean - best - 0.01 * std::abs(best);
    const double z = gain / sd;
    return gain * norm_cdf(z) + sd * norm_pdf(z);
}

} // namespace

auto bayes_next(std::span<const double> lower, std::span<const double> upper,
                std::span<const char> integer, const std::vector<std::vector<double>>& tried,
                std::span<const double> values, int batch, std::uint64_t seed)
    -> std::vector<std::vector<double>> {
    const std::size_t dims = lower.size();
    std::mt19937_64 rng(seed);
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    auto to_box = [&](const Point& u) {
        Point x(dims);
        for (std::size_t d = 0; d < dims; ++d) {
            x[d] = lower[d] + u[d] * (upper[d] - lower[d]);
            if (integer[d] != 0) {
                x[d] = std::clamp(std::round(x[d]), std::ceil(lower[d]), std::floor(upper[d]));
            }
        }
        return x;
    };
    auto to_unit = [&](const Point& x) {
        Point u(dims);
        for (std::size_t d = 0; d < dims; ++d) {
            u[d] = upper[d] > lower[d] ? (x[d] - lower[d]) / (upper[d] - lower[d]) : 0.0;
        }
        return u;
    };
    std::vector<Point> seen = tried; // in the box, as tried or chosen
    auto taken = [&](const Point& x) { return std::ranges::find(seen, x) != seen.end(); };
    std::vector<Point> out;
    const auto start = static_cast<std::size_t>(2 * dims + 1);
    if (tried.size() < start) { // the start: a Latin hypercube
        const int n = batch;
        std::vector<std::vector<double>> columns(dims);
        for (std::size_t d = 0; d < dims; ++d) {
            for (int i = 0; i < n; ++i) {
                columns[d].push_back((i + unit(rng)) / n);
            }
            std::ranges::shuffle(columns[d], rng);
        }
        for (int i = 0; i < n; ++i) {
            Point u(dims);
            for (std::size_t d = 0; d < dims; ++d) {
                u[d] = columns[d][static_cast<std::size_t>(i)];
            }
            if (Point x = to_box(u); !taken(x)) {
                seen.push_back(x);
                out.push_back(std::move(x));
            }
        }
        return out;
    }
    // A trial with no value (it failed) counts as the worst seen.
    double worst = std::numeric_limits<double>::infinity();
    for (const double v : values) {
        if (std::isfinite(v)) {
            worst = std::min(worst, v);
        }
    }
    if (!std::isfinite(worst)) {
        worst = 0.0;
    }
    std::vector<Point> xs;
    std::vector<double> ys;
    for (std::size_t i = 0; i < tried.size(); ++i) {
        xs.push_back(to_unit(tried[i]));
        ys.push_back(std::isfinite(values[i]) ? values[i] : worst);
    }
    for (int b = 0; b < batch; ++b) {
        const Process gp = fitted(xs, ys);
        const double best = *std::ranges::max_element(ys);
        // Candidates: uniform over the box, and near the best points so far.
        std::vector<std::size_t> order(xs.size());
        for (std::size_t i = 0; i < order.size(); ++i) {
            order[i] = i;
        }
        std::ranges::sort(order, [&](std::size_t a, std::size_t c) { return ys[a] > ys[c]; });
        std::normal_distribution<double> step(0.0, 0.05);
        std::vector<Point> candidates;
        for (int i = 0; i < 2000; ++i) {
            Point u(dims);
            for (double& v : u) {
                v = unit(rng);
            }
            candidates.push_back(std::move(u));
        }
        for (std::size_t r = 0; r < std::min<std::size_t>(5, order.size()); ++r) {
            for (int i = 0; i < 100; ++i) {
                Point u = xs[order[r]];
                for (double& v : u) {
                    v = std::clamp(v + step(rng), 0.0, 1.0);
                }
                candidates.push_back(std::move(u));
            }
        }
        double most = -1.0;
        Point chosen;
        for (const Point& u : candidates) {
            Point x = to_box(u);
            if (taken(x)) {
                continue;
            }
            const auto [mean, sd] = gp.predict(to_unit(x));
            if (const double ei = expected_improvement(mean, sd, best); ei > most) {
                most = ei;
                chosen = std::move(x);
            }
        }
        if (chosen.empty()) {
            break; // every candidate tried: the box is used up
        }
        // The constant liar: as if it scored the worst, so the next point goes elsewhere.
        seen.push_back(chosen);
        xs.push_back(to_unit(chosen));
        ys.push_back(worst);
        out.push_back(std::move(chosen));
    }
    return out;
}

} // namespace quant
