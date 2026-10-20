#include "surface.hpp"
#include "strategy_registry.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <optional>
#include <tuple>

namespace strategy {

namespace {

constexpr std::array<int, 3> kDays{7, 30, 90};

struct Point {
    double years;
    double atm_iv;
    std::optional<double> rr25;
    std::optional<double> bf25;
};

/** The two points around `years` (sorted by years), and how far between them it is. */
auto around(const std::vector<Point>& points, double years)
    -> std::optional<std::tuple<const Point*, const Point*, double>> {
    const auto upper = std::ranges::find_if(points, [&](const Point& p) { return p.years >= years; });
    if (upper == points.begin() || upper == points.end()) {
        return std::nullopt;
    }
    const Point& lo = *std::prev(upper);
    return std::tuple{&lo, &*upper, (years - lo.years) / (upper->years - lo.years)};
}

auto between(std::optional<double> lo, std::optional<double> hi, double share)
    -> std::optional<double> {
    return lo && hi ? std::optional(*lo + ((*hi - *lo) * share)) : std::nullopt;
}

} // namespace

void SurfaceStrategy::on_day() {
    measured_ = false;
    std::vector<std::pair<int, std::string>> listed;
    for (const std::string& symbol : portfolio()->chains_by_expiry(1, 400)) {
        listed.emplace_back(portfolio()->chain(symbol)->days_to_expiry(), symbol);
    }
    std::ranges::sort(listed);
    chains_.clear();
    for (const int days : kDays) {
        // The last before `days` and the first from it: the years from the measuring minute to an
        // expiry `days` out are a little over days / 365.
        const auto upper = std::ranges::find_if(listed, [&](const auto& c) { return c.first >= days; });
        if (upper != listed.begin()) {
            chains_.push_back(std::prev(upper)->second);
        }
        if (upper != listed.end()) {
            chains_.push_back(upper->second);
        }
    }
    std::ranges::sort(chains_);
    const auto [first, last] = std::ranges::unique(chains_);
    chains_.erase(first, last);
    subscribe_chains(chains_);
}

void SurfaceStrategy::on_timer() {
    const int close = static_cast<int>((calendar().session_close(day()) - calendar().open).count());
    if (!measured_ && minute() >= close - setting_.before_close) {
        measured_ = true;
        measure();
    }
}

void SurfaceStrategy::measure() {
    if (const auto u = underlying(); u && u->mid() > 0.0) {
        write_value("spot", u->mid());
    }
    std::vector<Point> points;
    for (const std::string& symbol : chains_) {
        const auto chain = find_chain(symbol);
        if (!chain) {
            continue;
        }
        const std::optional<double> atm = chain->atm_iv();
        const double years = model::years_to_expiry(now(), chain->expiry());
        if (!atm || years <= 0.0) {
            continue;
        }
        const std::optional<double> call = chain->smile_iv_at_delta(0.25);
        const std::optional<double> put = chain->smile_iv_at_delta(-0.25);
        points.push_back({.years = years,
                          .atm_iv = *atm,
                          .rr25 = call && put ? std::optional(*call - *put) : std::nullopt,
                          .bf25 = call && put ? std::optional(((*call + *put) / 2.0) - *atm)
                                              : std::nullopt});
    }
    std::ranges::sort(points, {}, &Point::years);

    std::array<std::optional<double>, kDays.size()> atm_ivs;
    for (std::size_t i = 0; i < kDays.size(); ++i) {
        const double years = kDays[i] / 365.0;
        const auto found = around(points, years);
        if (!found) {
            continue;
        }
        const auto [lo, hi, share] = *found;
        const double w = (lo->atm_iv * lo->atm_iv * lo->years) +
                         (((hi->atm_iv * hi->atm_iv * hi->years) -
                           (lo->atm_iv * lo->atm_iv * lo->years)) *
                          share);
        if (w <= 0.0) {
            continue;
        }
        atm_ivs[i] = std::sqrt(w / years);
        write_value(std::format("atm_iv_{}d", kDays[i]), *atm_ivs[i]);
        if (kDays[i] == 30) {
            if (const auto rr = between(lo->rr25, hi->rr25, share)) {
                write_value("rr25_30d", *rr);
            }
            if (const auto bf = between(lo->bf25, hi->bf25, share)) {
                write_value("bf25_30d", *bf);
            }
        }
    }
    if (atm_ivs[1] && atm_ivs[2]) {
        write_value("term_slope", *atm_ivs[2] - *atm_ivs[1]);
    }
}

REGISTER_STRATEGY(SurfaceStrategy);

} // namespace strategy
