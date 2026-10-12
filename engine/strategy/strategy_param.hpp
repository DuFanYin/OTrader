#pragma once

#include <string_view>

namespace strategy {

/** A parameter a strategy declares: the engine checks settings against it, and it is the range a
 * search may vary (entry_backtest --list-strategies prints them). */
struct StrategyParam {
    std::string_view name;
    double default_value = 0.0;
    double min = 0.0;
    double max = 0.0;
    bool integer = false;
};

} // namespace strategy
