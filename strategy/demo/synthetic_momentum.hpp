#pragma once

/**
 * SyntheticMomentumStrategy: buys, late in the day, a 0DTE option on the side the morning moved.
 * The edge the synthetic days plant with momentum (app/server optra/data/synthetic_days.py:
 * the last 30 minutes drift by M times the first 30 minutes' return), so the research process's
 * exam (optra.research.exam) knows what it should find. On real days it is only an example.
 */

#include "zero_dte_buyer.hpp"

#include <chrono>
#include <optional>

namespace strategy {

class SyntheticMomentumStrategy final : public ZeroDteBuyer {
  public:
    struct Setting : ZeroDteBuyer::Setting {
        Param<int> signal_minutes{"signal_minutes", 30, 1, 120}; // the morning move: open to 10:00
        Param<int> entry_minute{"entry_minute", 360, 30, 389};   // 15:30
    };
    using ZeroDteBuyer::ZeroDteBuyer;

  protected:
    std::optional<model::OptionType> entry_signal(int minute) override;

  private:
    const Setting setting_ = read_setting<Setting>();
    std::chrono::year_month_day signal_day_{};
    double signal_ = 0.0; // the morning's return; 0 until measured
};

} // namespace strategy
