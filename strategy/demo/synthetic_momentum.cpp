#include "synthetic_momentum.hpp"
#include "strategy_registry.hpp"

namespace strategy {

auto SyntheticMomentumStrategy::entry_signal(int minute) -> std::optional<model::OptionType> {
    if (signal_day_ != day()) {
        signal_day_ = day();
        signal_ = 0.0;
    }
    if (minute <= setting_.signal_minutes || !saw_open()) {
        return std::nullopt;
    }
    if (signal_ == 0.0 && open_price() > 0.0) {
        signal_ = price() / open_price() - 1.0;
    }
    if (minute < setting_.entry_minute || signal_ == 0.0) {
        return std::nullopt;
    }
    return signal_ > 0.0 ? model::OptionType::CALL : model::OptionType::PUT;
}

REGISTER_STRATEGY(SyntheticMomentumStrategy);

} // namespace strategy
