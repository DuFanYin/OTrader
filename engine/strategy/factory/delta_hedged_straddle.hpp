#pragma once

#include "template.hpp"

#include <span>
#include <string>
#include <unordered_map>

namespace strategy {

/**
 * DeltaHedgedStraddle: holds one ATM straddle (bought or sold) and leaves its delta to the
 * HedgeEngine, which trades the underlying to keep it within delta_range shares of zero. What it
 * earns is then the gap between the vol the options were priced at and the vol the underlying
 * moves at, not the direction it moves in. With hedge 0 it holds the same straddle unhedged.
 */
class DeltaHedgedStraddleStrategy final : public OptionStrategyTemplate {
  public:
    DeltaHedgedStraddleStrategy(StrategyContext* context, const std::string& strategy_name,
                                const std::string& portfolio_name,
                                const std::unordered_map<std::string, double>& setting);

    static std::span<const StrategyParam> parameters();

    void on_init_logic() override;
    void on_stop_logic() override;
    void on_timer_logic() override;

  private:
    int position_size_;
    model::Direction side_;
    int min_dte_;
    int max_dte_;
    bool hedge_;
    int hedge_timer_;
    int delta_range_;
    std::string chain_;
    bool entered_ = false;
};

} // namespace strategy
