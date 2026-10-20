#pragma once

// The one Nelder-Mead: the SVI fit (each chain, each minute) and the Heston calibration.

#include <algorithm>
#include <array>
#include <cstddef>

namespace quant {

template <std::size_t N>
struct Minimum {
    std::array<double, N> x{};
    double value = 0.0;
    int iterations = 0;
    bool converged = false;
};

/** Nelder-Mead on N unknowns within [lower, upper] (a point outside clamped to it), from x0 a
 * first step along each; until the simplex's values are within tol, or `iterations`. On the stack,
 * the simplex ordered stably (the same values, the same order): the same answer every time. */
template <std::size_t N, typename F>
auto minimise(const F& f, std::array<double, N> x0, const std::array<double, N>& step,
              const std::array<double, N>& lower, const std::array<double, N>& upper, double tol,
              int iterations) -> Minimum<N> {
    using Point = std::array<double, N>;
    const auto clamp = [&](Point& p) {
        for (std::size_t i = 0; i < N; ++i) {
            p[i] = std::clamp(p[i], lower[i], upper[i]);
        }
    };
    std::array<Point, N + 1> x;
    std::array<double, N + 1> v;
    clamp(x0);
    x[0] = x0;
    for (std::size_t i = 0; i < N; ++i) {
        x[i + 1] = x0;
        x[i + 1][i] += x0[i] + step[i] <= upper[i] ? step[i] : -step[i];
        clamp(x[i + 1]);
    }
    for (std::size_t i = 0; i <= N; ++i) {
        v[i] = f(x[i]);
    }
    std::array<std::size_t, N + 1> order;
    const auto sort = [&] {
        for (std::size_t i = 0; i <= N; ++i) {
            order[i] = i;
        }
        for (std::size_t i = 1; i <= N; ++i) { // insertion: the same order for the same values
            for (std::size_t j = i; j > 0 && v[order[j]] < v[order[j - 1]]; --j) {
                std::swap(order[j], order[j - 1]);
            }
        }
    };
    const auto toward = [&](const Point& from, const Point& to, double t) {
        Point p;
        for (std::size_t i = 0; i < N; ++i) {
            p[i] = from[i] + (t * (to[i] - from[i]));
        }
        clamp(p);
        return p;
    };
    sort();
    int it = 0;
    for (; it < iterations && v[order[N]] - v[order[0]] > tol; ++it) {
        const std::size_t worst = order[N];
        Point centre{};
        for (std::size_t j = 0; j < N; ++j) {
            for (std::size_t i = 0; i < N; ++i) {
                centre[i] += x[order[j]][i] / static_cast<double>(N);
            }
        }
        const Point reflected = toward(centre, x[worst], -1.0);
        const double fr = f(reflected);
        if (fr < v[order[0]]) {
            const Point expanded = toward(centre, x[worst], -2.0);
            const double fe = f(expanded);
            x[worst] = fe < fr ? expanded : reflected;
            v[worst] = fe < fr ? fe : fr; // the value of the point kept, NaN or not
        } else if (fr < v[order[N - 1]]) {
            x[worst] = reflected;
            v[worst] = fr;
        } else {
            const bool outside = fr < v[worst];
            const Point contracted = toward(centre, outside ? reflected : x[worst], 0.5);
            const double fc = f(contracted);
            if (fc < (outside ? fr : v[worst])) {
                x[worst] = contracted;
                v[worst] = fc;
            } else {
                const std::size_t best = order[0];
                for (std::size_t i = 0; i <= N; ++i) {
                    if (i != best) {
                        x[i] = toward(x[best], x[i], 0.5);
                        v[i] = f(x[i]);
                    }
                }
            }
        }
        sort();
    }
    return {.x = x[order[0]], .value = v[order[0]], .iterations = it,
            .converged = v[order[N]] - v[order[0]] <= tol};
}

} // namespace quant
