/** Raw SVI: Gatheral's butterfly check, and the fit to a smile's total variances (Zeliade's
 * quasi-explicit fit). No allocation once a thread has fitted a smile as large. */

#include "quant/engine_api.h"
#include "quant/optimizer/nelder_mead.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <span>
#include <utility>
#include <vector>

namespace quant {

namespace {

constexpr int kMinPoints = 5;
constexpr int kButterflyPoints = 101;
// The (m, sigma) tried before Nelder-Mead starts from the best: m over the points' span of
// log-moneyness and as far again either side, sigma from a thousandth of that span to ten times it.
constexpr int kGridM = 9;
constexpr int kGridSigma = 7;
constexpr double kSigmaLow = 1e-3;
constexpr double kSigmaHigh = 10.0;
constexpr int kIterations = 200;
// Fitted again with butterfly arbitrage penalised: g's shortfall below 0, squared, at these points.
constexpr int kPenalisedIterations = 600;
constexpr int kPenaltyPoints = 41;
constexpr double kPenalty = 1e4;
constexpr double kInfinity = std::numeric_limits<double>::infinity();

/** Gatheral's g at k: the risk-neutral density over the normal one; NaN where the total variance
 * is not positive. */
auto density_ratio(const Svi& s, double k) -> double {
    const double x = k - s.m;
    const double root = std::sqrt((x * x) + (s.sigma * s.sigma));
    const double w = s.a + (s.b * ((s.rho * x) + root));
    if (!(w > 0.0)) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    const double w1 = s.b * (s.rho + (x / root));
    const double w2 = s.b * s.sigma * s.sigma / (root * root * root);
    const double u = 1.0 - (k * w1 / (2.0 * w));
    return (u * u) - ((w1 * w1 / 4.0) * ((1.0 / w) + 0.25)) + (w2 / 2.0);
}

/** The points in the fit's units: total variance over the largest one, each weighted by one over
 * its half spread squared (in the same units). A thread's, kept for its next fit. */
struct Points {
    std::vector<double> k;
    std::vector<double> w;
    std::vector<double> weight;
    double scale = 0.0; // the largest total variance
    double lo = 0.0;    // the least and greatest k
    double hi = 0.0;
};

struct Linear {
    std::array<double, 3> x{}; // a, d, c
    double value = kInfinity;  // the mean of the weighted squared residuals
};

// The constraints on (a, d, c) as rows of A x <= b: a + c - |d| >= 0 (the total variance never
// negative: its least, a + sqrt(c^2 - d^2), is at least that), |d| <= c, and c + |d| <= 4 sigma
// (b (1 + |rho|) <= 4, b in total variance; over the points' scale).
constexpr std::array<std::array<double, 3>, 6> kRows{{{-1.0, 1.0, -1.0},
                                                      {-1.0, -1.0, -1.0},
                                                      {0.0, 1.0, -1.0},
                                                      {0.0, -1.0, -1.0},
                                                      {0.0, 1.0, 1.0},
                                                      {0.0, -1.0, 1.0}}};
// The sets of them that can bind at once: one, two or three, without two parallel ones (2 and 5,
// 3 and 4). Each its rows, -1 past the last.
constexpr auto kBindingSets = [] {
    std::array<std::array<int, 3>, 31> sets{};
    std::size_t n = 0;
    const auto parallel = [](int i, int j) { return (i == 2 && j == 5) || (i == 3 && j == 4); };
    for (int i = 0; i < 6; ++i) {
        sets[n++] = {i, -1, -1};
    }
    for (int i = 0; i < 6; ++i) {
        for (int j = i + 1; j < 6; ++j) {
            if (!parallel(i, j)) {
                sets[n++] = {i, j, -1};
            }
        }
    }
    for (int i = 0; i < 6; ++i) {
        for (int j = i + 1; j < 6; ++j) {
            for (int l = j + 1; l < 6; ++l) {
                if (!parallel(i, j) && !parallel(i, l) && !parallel(j, l)) {
                    sets[n++] = {i, j, l};
                }
            }
        }
    }
    return sets;
}();

/** For (m, sigma), (a, d, c) of w = a + d y + c sqrt(y^2 + 1), y = (k - m) / sigma, by least
 * squares within kRows: unbound when that is within them, else of the solutions with each set of
 * kBindingSets binding (x = xu - P (A P)^-1 (A xu - b), xu unbound, P = H^-1 A'), the best within
 * all of them. */
auto fit_linear(const Points& p, double m, double sigma) -> Linear {
    double s1 = 0.0, sy = 0.0, sz = 0.0, syy = 0.0, syz = 0.0, szz = 0.0;
    double sw = 0.0, syw = 0.0, szw = 0.0, sww = 0.0;
    const double inverse_sigma = 1.0 / sigma;
    for (std::size_t i = 0; i < p.k.size(); ++i) {
        const double y = (p.k[i] - m) * inverse_sigma;
        const double z = std::sqrt((y * y) + 1.0);
        const double o = p.weight[i];
        const double ow = o * p.w[i];
        s1 += o;
        sy += o * y;
        sz += o * z;
        syy += o * y * y;
        syz += o * y * z;
        szz += o * z * z;
        sw += ow;
        syw += ow * y;
        szw += ow * z;
        sww += ow * p.w[i];
    }
    const double n = static_cast<double>(p.k.size());
    const std::array<std::array<double, 3>, 3> h{
        {{s1 / n, sy / n, sz / n}, {sy / n, syy / n, syz / n}, {sz / n, syz / n, szz / n}}};
    const std::array<double, 3> g{sw / n, syw / n, szw / n};
    const double ww = sww / n;
    // H^-1 by cofactors.
    const double c00 = (h[1][1] * h[2][2]) - (h[1][2] * h[2][1]);
    const double c01 = (h[1][2] * h[2][0]) - (h[1][0] * h[2][2]);
    const double c02 = (h[1][0] * h[2][1]) - (h[1][1] * h[2][0]);
    const double det = (h[0][0] * c00) + (h[0][1] * c01) + (h[0][2] * c02);
    if (!(std::abs(det) > 1e-14 * h[0][0] * h[1][1] * h[2][2])) {
        return {};
    }
    const double inv = 1.0 / det;
    const std::array<std::array<double, 3>, 3> hi{
        {{c00 * inv, ((h[0][2] * h[2][1]) - (h[0][1] * h[2][2])) * inv,
          ((h[0][1] * h[1][2]) - (h[0][2] * h[1][1])) * inv},
         {c01 * inv, ((h[0][0] * h[2][2]) - (h[0][2] * h[2][0])) * inv,
          ((h[0][2] * h[1][0]) - (h[0][0] * h[1][2])) * inv},
         {c02 * inv, ((h[0][1] * h[2][0]) - (h[0][0] * h[2][1])) * inv,
          ((h[0][0] * h[1][1]) - (h[0][1] * h[1][0])) * inv}}};
    const auto times = [](const std::array<std::array<double, 3>, 3>& a,
                          const std::array<double, 3>& v) {
        return std::array<double, 3>{(a[0][0] * v[0]) + (a[0][1] * v[1]) + (a[0][2] * v[2]),
                                     (a[1][0] * v[0]) + (a[1][1] * v[1]) + (a[1][2] * v[2]),
                                     (a[2][0] * v[0]) + (a[2][1] * v[1]) + (a[2][2] * v[2])};
    };
    const auto dot = [](const std::array<double, 3>& a, const std::array<double, 3>& b) {
        return (a[0] * b[0]) + (a[1] * b[1]) + (a[2] * b[2]);
    };
    const double wings = 4.0 * sigma / p.scale;
    const std::array<double, 6> bound{0.0, 0.0, 0.0, 0.0, wings, wings};
    const auto within = [&](const std::array<double, 3>& x) {
        for (std::size_t r = 0; r < kRows.size(); ++r) {
            if (dot(kRows[r], x) > bound[r] + (1e-10 * (1.0 + bound[r]))) {
                return false;
            }
        }
        return true;
    };
    const auto value = [&](const std::array<double, 3>& x) {
        return ww - (2.0 * dot(g, x)) + dot(x, times(h, x));
    };
    const std::array<double, 3> xu = times(hi, g);
    if (within(xu)) {
        return {xu, value(xu)};
    }
    std::array<std::array<double, 3>, 6> pc{}; // P's columns
    std::array<double, 6> excess{};            // A xu - b
    for (std::size_t r = 0; r < kRows.size(); ++r) {
        pc[r] = times(hi, kRows[r]);
        excess[r] = dot(kRows[r], xu) - bound[r];
    }
    Linear best;
    for (const std::array<int, 3>& set : kBindingSets) {
        const auto i = static_cast<std::size_t>(set[0]);
        std::array<double, 3> nu{}; // (A P)^-1 (A xu - b) over the set
        if (set[1] < 0) {
            const double m00 = dot(kRows[i], pc[i]);
            if (!(m00 > 0.0)) {
                continue;
            }
            nu[0] = excess[i] / m00;
        } else if (set[2] < 0) {
            const auto j = static_cast<std::size_t>(set[1]);
            const double m00 = dot(kRows[i], pc[i]);
            const double m01 = dot(kRows[i], pc[j]);
            const double m11 = dot(kRows[j], pc[j]);
            const double d = (m00 * m11) - (m01 * m01);
            if (!(d > 1e-14 * m00 * m11)) {
                continue;
            }
            nu[0] = ((m11 * excess[i]) - (m01 * excess[j])) / d;
            nu[1] = ((m00 * excess[j]) - (m01 * excess[i])) / d;
        } else {
            const std::array<std::size_t, 3> r{i, static_cast<std::size_t>(set[1]),
                                               static_cast<std::size_t>(set[2])};
            std::array<std::array<double, 3>, 3> a{};
            for (std::size_t u = 0; u < 3; ++u) {
                for (std::size_t v = 0; v < 3; ++v) {
                    a[u][v] = dot(kRows[r[u]], pc[r[v]]);
                }
            }
            const double e00 = (a[1][1] * a[2][2]) - (a[1][2] * a[2][1]);
            const double e01 = (a[1][2] * a[2][0]) - (a[1][0] * a[2][2]);
            const double e02 = (a[1][0] * a[2][1]) - (a[1][1] * a[2][0]);
            const double d = (a[0][0] * e00) + (a[0][1] * e01) + (a[0][2] * e02);
            if (!(std::abs(d) > 1e-14 * a[0][0] * a[1][1] * a[2][2])) {
                continue;
            }
            const std::array<double, 3> rhs{excess[r[0]], excess[r[1]], excess[r[2]]};
            // Cramer's rule.
            for (std::size_t u = 0; u < 3; ++u) {
                std::array<std::array<double, 3>, 3> b = a;
                for (std::size_t v = 0; v < 3; ++v) {
                    b[v][u] = rhs[v];
                }
                nu[u] = ((b[0][0] * ((b[1][1] * b[2][2]) - (b[1][2] * b[2][1]))) +
                         (b[0][1] * ((b[1][2] * b[2][0]) - (b[1][0] * b[2][2]))) +
                         (b[0][2] * ((b[1][0] * b[2][1]) - (b[1][1] * b[2][0])))) /
                        d;
            }
        }
        std::array<double, 3> x = xu;
        for (std::size_t u = 0; u < 3 && set[u] >= 0; ++u) {
            const std::array<double, 3>& column = pc[static_cast<std::size_t>(set[u])];
            for (std::size_t c = 0; c < 3; ++c) {
                x[c] -= column[c] * nu[u];
            }
        }
        if (!within(x)) {
            continue;
        }
        if (const double v = value(x); v < best.value) {
            best = {x, v};
        }
    }
    return best;
}

/** The mean of the weighted squared residuals of `s` (in the points' units). */
auto residuals(const Points& p, const Svi& s) -> double {
    double sum = 0.0;
    for (std::size_t i = 0; i < p.k.size(); ++i) {
        const double r = s.total_variance(p.k[i]) - p.w[i];
        sum += p.weight[i] * r * r;
    }
    return sum / static_cast<double>(p.k.size());
}

/** In total variance, of one in the points' units (g is not the same at another scale). */
auto unscaled(const Points& p, const Svi& unit) -> Svi {
    return {.a = unit.a * p.scale, .b = unit.b * p.scale, .rho = unit.rho, .m = unit.m,
            .sigma = unit.sigma};
}

/** Fitted again from `start` (in the points' units), all five at once, with butterfly arbitrage
 * within the points penalised. */
auto fit_penalised(const Points& p, const Svi& start) -> Svi {
    const double span = p.hi - p.lo;
    const auto svi_of = [span](const std::array<double, 5>& x) {
        return Svi{.a = x[0], .b = x[1] / span, .rho = x[2], .m = x[3] * span,
                   .sigma = std::exp(x[4])};
    };
    const auto objective = [&](const std::array<double, 5>& x) {
        const Svi s = svi_of(x);
        const Svi total = unscaled(p, s);
        double shortfall = 0.0;
        for (int j = 0; j < kPenaltyPoints; ++j) {
            const double g = density_ratio(total, p.lo + (span * j / (kPenaltyPoints - 1)));
            if (std::isnan(g)) {
                return 1e30;
            }
            shortfall += g < 0.0 ? g * g : 0.0;
        }
        return residuals(p, s) + (kPenalty * shortfall);
    };
    const std::array<double, 5> x0{start.a, start.b * span, start.rho, start.m / span,
                                   std::log(start.sigma)};
    const std::array<double, 5> step{0.1, std::max(0.1 * x0[1], 0.1), 0.1, 0.1, 0.3};
    const std::array<double, 5> lower{-1.0, 0.0, -0.999, (p.lo - span) / span,
                                      std::log(span * kSigmaLow)};
    const std::array<double, 5> upper{1.0, 4.0 * span / p.scale, 0.999, (p.hi + span) / span,
                                      std::log(span * kSigmaHigh)};
    const double tol = 1e-8 * (1.0 + objective(x0));
    return svi_of(minimise(objective, x0, step, lower, upper, tol, kPenalisedIterations).x);
}

} // namespace

auto Svi::butterfly_free(double lo, double hi) const -> bool {
    for (int j = 0; j < kButterflyPoints; ++j) {
        if (!(density_ratio(*this, lo + ((hi - lo) * j / (kButterflyPoints - 1))) >= 0.0)) {
            return false;
        }
    }
    return true;
}

auto Svi::calendar_free(const Svi& later, double lo, double hi) const -> bool {
    for (int j = 0; j < kButterflyPoints; ++j) {
        const double k = lo + ((hi - lo) * j / (kButterflyPoints - 1));
        if (!(later.total_variance(k) >= total_variance(k))) {
            return false;
        }
    }
    return true;
}

auto svi_fit(std::span<const double> k, std::span<const double> w,
             std::span<const double> half_spread) -> SviFit {
    thread_local Points p;
    p.k.clear();
    p.w.clear();
    p.weight.clear();
    p.scale = 0.0;
    const std::size_t n = std::min({k.size(), w.size(), half_spread.size()});
    for (std::size_t i = 0; i < n; ++i) {
        if (std::isfinite(k[i]) && w[i] > 0.0 && std::isfinite(w[i]) && half_spread[i] > 0.0 &&
            std::isfinite(half_spread[i])) {
            p.k.push_back(k[i]);
            p.w.push_back(w[i]);
            p.weight.push_back(half_spread[i]);
            p.scale = std::max(p.scale, w[i]);
        }
    }
    if (std::ssize(p.k) < kMinPoints) {
        return {};
    }
    p.lo = std::ranges::min(p.k);
    p.hi = std::ranges::max(p.k);
    const double span = p.hi - p.lo;
    if (!(span > 0.0)) {
        return {};
    }
    for (std::size_t i = 0; i < p.k.size(); ++i) {
        p.w[i] /= p.scale;
        const double h = p.weight[i] / p.scale;
        p.weight[i] = 1.0 / (h * h);
    }

    double best = kInfinity;
    std::array<double, 2> x0{};
    for (int i = 0; i < kGridM; ++i) {
        const double m = p.lo - span + (3.0 * span * i / (kGridM - 1));
        for (int j = 0; j < kGridSigma; ++j) {
            const double sigma =
                span * kSigmaLow * std::pow(kSigmaHigh / kSigmaLow, j / (kGridSigma - 1.0));
            if (const double v = fit_linear(p, m, sigma).value; v < best) {
                best = v;
                x0 = {m / span, std::log(sigma)};
            }
        }
    }
    if (!std::isfinite(best)) {
        return {};
    }
    // In (m / span, ln sigma); a first step of half the grid's.
    const std::array<double, 2> step{1.5 / (kGridM - 1),
                                     0.5 * std::log(kSigmaHigh / kSigmaLow) / (kGridSigma - 1)};
    const std::array<double, 2> lower{(p.lo - span) / span, std::log(span * kSigmaLow)};
    const std::array<double, 2> upper{(p.hi + span) / span, std::log(span * kSigmaHigh)};
    const std::array<double, 2> x =
        minimise(
            [&](const std::array<double, 2>& u) {
                return fit_linear(p, u[0] * span, std::exp(u[1])).value;
            },
            x0, step, lower, upper, 1e-8 * (1.0 + best), kIterations)
            .x;
    const double m = x[0] * span;
    const double sigma = std::exp(x[1]);
    const Linear lin = fit_linear(p, m, sigma);
    const double c = lin.x[2];
    Svi unit{.a = lin.x[0], .b = c / sigma, .rho = c > 0.0 ? lin.x[1] / c : 0.0, .m = m,
             .sigma = sigma};
    double value = lin.value;
    bool free = unscaled(p, unit).butterfly_free(p.lo, p.hi);
    if (!free) {
        const Svi again = fit_penalised(p, unit);
        free = unscaled(p, again).butterfly_free(p.lo, p.hi);
        if (free) {
            unit = again;
            value = residuals(p, unit);
        }
    }
    return {.svi = unscaled(p, unit),
            .error = std::sqrt(std::max(value, 0.0)),
            .points = static_cast<int>(p.k.size()),
            .butterfly_free = free};
}

} // namespace quant
