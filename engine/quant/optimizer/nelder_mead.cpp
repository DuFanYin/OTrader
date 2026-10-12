#include <algorithm>
#include <cmath>
#include <functional>
#include <numeric>
#include <span>
#include <vector>

namespace quant {

struct OptResult {
    std::vector<double> x;
    double fval      = 1e30;
    int    iterations = 0;
    bool   converged  = false;
};

// Nelder-Mead simplex minimizer. Bounds enforced by reflection.
OptResult nelder_mead(
    std::function<double(std::span<const double>)> objective,
    std::span<const double> x0,
    std::span<const double> lower,
    std::span<const double> upper,
    double tol,
    int max_iter
) {
    const int n = static_cast<int>(x0.size());
    OptResult result;
    if (n == 0) return result;

    auto clamp = [&](std::vector<double>& x) {
        for (int k = 0; k < n; ++k)
            x[k] = std::clamp(x[k], lower[k], upper[k]);
    };

    auto eval = [&](std::vector<double>& x) -> double {
        clamp(x);
        return objective(std::span<const double>(x));
    };

    // Build initial simplex: x0 + perturbation in each dimension
    const int np1 = n + 1;
    std::vector<std::vector<double>> simplex(np1, std::vector<double>(x0.begin(), x0.end()));
    std::vector<double> fvals(np1);
    for (int i = 0; i < n; ++i) {
        const double perturb = std::abs(x0[i]) > 1e-8 ? x0[i] * 0.15 : 0.1;
        simplex[i + 1][i] = std::clamp(x0[i] + perturb, lower[i], upper[i]);
    }
    for (int i = 0; i < np1; ++i) fvals[i] = eval(simplex[i]);

    // NM coefficients
    const double alpha = 1.0, gamma = 2.0, rho = 0.5, sigma = 0.5;

    for (int iter = 0; iter < max_iter; ++iter) {
        // Sort simplex by function value
        std::vector<int> idx(np1);
        std::iota(idx.begin(), idx.end(), 0);
        std::sort(idx.begin(), idx.end(), [&](int a, int b) { return fvals[a] < fvals[b]; });

        if (fvals[idx[0]] < result.fval) {
            result.x    = simplex[idx[0]];
            result.fval = fvals[idx[0]];
        }

        // Convergence: range of function values
        if (fvals[idx[n]] - fvals[idx[0]] < tol) {
            result.iterations = iter;
            result.converged  = true;
            return result;
        }

        // Centroid (excluding worst)
        std::vector<double> centroid(n, 0.0);
        for (int i = 0; i < n; ++i)
            for (int k = 0; k < n; ++k)
                centroid[k] += simplex[idx[i]][k] / n;

        // Reflection
        std::vector<double> xr(n);
        for (int k = 0; k < n; ++k) xr[k] = centroid[k] + alpha * (centroid[k] - simplex[idx[n]][k]);
        const double fr = eval(xr);

        if (fr < fvals[idx[0]]) {
            // Expansion
            std::vector<double> xe(n);
            for (int k = 0; k < n; ++k) xe[k] = centroid[k] + gamma * (xr[k] - centroid[k]);
            const double fe = eval(xe);
            if (fe < fr) { simplex[idx[n]] = xe; fvals[idx[n]] = fe; }
            else         { simplex[idx[n]] = xr; fvals[idx[n]] = fr; }
        } else if (fr < fvals[idx[n - 1]]) {
            simplex[idx[n]] = xr; fvals[idx[n]] = fr;
        } else {
            // Contraction
            const bool outside = fr < fvals[idx[n]];
            std::vector<double> xc(n);
            const auto& ref = outside ? xr : simplex[idx[n]];
            for (int k = 0; k < n; ++k) xc[k] = centroid[k] + rho * (ref[k] - centroid[k]);
            const double fc = eval(xc);
            if (fc < (outside ? fr : fvals[idx[n]])) {
                simplex[idx[n]] = xc; fvals[idx[n]] = fc;
            } else {
                // Shrink
                for (int i = 1; i < np1; ++i) {
                    for (int k = 0; k < n; ++k)
                        simplex[idx[i]][k] = simplex[idx[0]][k]
                                           + sigma * (simplex[idx[i]][k] - simplex[idx[0]][k]);
                    fvals[idx[i]] = eval(simplex[idx[i]]);
                }
            }
        }
    }

    result.iterations = max_iter;
    return result;
}

}  // namespace quant
