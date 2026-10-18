#include "quant/kernel.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <span>
#include <utility>
#include <vector>

namespace quant {

auto realized_vol(std::span<const double> closes, double periods_per_year) -> double {
    std::vector<double> returns;
    for (std::size_t i = 1; i < closes.size(); ++i) {
        if (closes[i - 1] > 0.0 && closes[i] > 0.0) {
            returns.push_back(std::log(closes[i] / closes[i - 1]));
        }
    }
    if (returns.size() < 2) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    return std::sqrt(sample_variance(returns) * periods_per_year);
}

auto percentile_rank(std::span<const double> history, double x) -> double {
    if (history.empty() || std::isnan(x)) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    const auto below = std::ranges::count_if(history, [x](double v) { return v <= x; });
    return static_cast<double>(below) / static_cast<double>(history.size());
}

auto iv_at_delta(std::span<const double> deltas, std::span<const double> ivs, double target)
    -> double {
    std::vector<std::pair<double, double>> points; // |delta|, IV
    for (std::size_t i = 0; i < std::min(deltas.size(), ivs.size()); ++i) {
        if (ivs[i] > 0.0 && std::isfinite(deltas[i]) && deltas[i] != 0.0) {
            points.emplace_back(std::abs(deltas[i]), ivs[i]);
        }
    }
    std::ranges::sort(points);
    for (std::size_t i = 1; i < points.size(); ++i) {
        const auto [d0, v0] = points[i - 1];
        const auto [d1, v1] = points[i];
        if (d0 <= target && target <= d1) {
            return d1 > d0 ? v0 + (v1 - v0) * (target - d0) / (d1 - d0) : v0;
        }
    }
    return std::numeric_limits<double>::quiet_NaN();
}

} // namespace quant
