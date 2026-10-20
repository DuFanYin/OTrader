#include "model/expiry.hpp"
#include "model/utility.hpp"
#include <algorithm>
#include <cmath>

namespace model {

auto intrinsic(const OptionId& id, double price) -> double {
    const double value = id.type == OptionType::CALL ? price - id.strike : id.strike - price;
    return round_digits(std::max(value, 0.0), 2);
}

auto settle_at_expiry(const OptionId& id, double price, bool physical, double multiplier)
    -> ExpirySettlement {
    const double value = intrinsic(id, price);
    if (!physical) {
        return {.close = value};
    }
    if (value < kExerciseThreshold) {
        return {};
    }
    return {.shares = id.type == OptionType::CALL ? multiplier : -multiplier};
}

auto worst_at_expiry(std::span<const ExpiryLeg> legs) -> ExpiryWorst {
    const auto value = [&](double s) {
        double v = 0.0;
        for (const ExpiryLeg& l : legs) {
            v += l.units * (l.type == OptionType::CALL ? std::max(s - l.strike, 0.0)
                                                       : std::max(l.strike - s, 0.0));
        }
        return v;
    };
    ExpiryWorst w{.worst = value(0.0)};
    for (const ExpiryLeg& l : legs) {
        w.worst = std::min(w.worst, value(l.strike));
        w.calls_net += l.type == OptionType::CALL ? l.units : 0.0;
    }
    return w;
}

} // namespace model
