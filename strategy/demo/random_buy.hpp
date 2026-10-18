#pragma once

/**
 * RandomBuyStrategy: the no-edge control every strategy must beat: buys a 0DTE option at a minute
 * and side drawn from the day and seed, held hold_minutes.
 */

#include "zero_dte_buyer.hpp"

#include <chrono>

namespace strategy {

class RandomBuyStrategy final : public ZeroDteBuyer {
  public:
    struct Setting : ZeroDteBuyer::Setting {
        Param<int> seed{"seed", 1, 0, 1'000'000};
        Param<int> earliest_minute{"earliest_minute", 30, 1, 389};
        Param<int> latest_minute{"latest_minute", 330, 1, 389};
        Param<int> hold_minutes{"hold_minutes", 30, 1, 389};
    };
    using ZeroDteBuyer::ZeroDteBuyer;

  protected:
    std::optional<model::OptionType> entry_signal(int minute) override;
    bool exit_due(int minute) const override;

  private:
    void draw(std::chrono::year_month_day day);

    const Setting setting_ = read_setting<Setting>();
    std::chrono::year_month_day drawn_for_{};
    int draw_minute_ = -1;
    model::OptionType draw_side_ = model::OptionType::CALL;
};

} // namespace strategy
