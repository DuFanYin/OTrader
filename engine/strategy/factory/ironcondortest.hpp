#pragma once

#include "template.hpp"

#include <chrono>
#include <cmath>
#include <span>
#include <vector>

namespace strategy {

class IronCondorTestStrategy final : public OptionStrategyTemplate {
  public:
    IronCondorTestStrategy(StrategyContext* context, const std::string& strategy_name,
                           const std::string& portfolio_name,
                           const std::unordered_map<std::string, double>& setting);

    static std::span<const StrategyParam> parameters();

    void on_init_logic() override;
    void on_stop_logic() override;
    void on_timer_logic() override;

  private:
    void enter_iron_condor(const model::OptionData* call, const model::OptionData* put,
                           double entry_price, const std::string& reason);
    void enter_atm_iron_condor();
    void reset_position();

    int position_size_;
    std::vector<std::string> chain_symbols_;
    int trade_count_ = 0;
    int step_in_cycle_ = 0;
};

} // namespace strategy
