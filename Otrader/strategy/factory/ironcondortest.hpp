#pragma once

#include "../core/engine_option_strategy.hpp"
#include "template.hpp"

#include <chrono>
#include <cmath>
#include <vector>

namespace strategy_cpp {

class IronCondorTestStrategy final : public OptionStrategyTemplate {
  public:
    IronCondorTestStrategy(core::OptionStrategyEngine* strategy_engine,
                           const std::string& strategy_name, const std::string& portfolio_name,
                           const std::unordered_map<std::string, double>& setting);

    void on_init_logic() override;
    void on_stop_logic() override;
    void on_timer_logic() override;

  private:
    void enter_iron_condor(utilities::OptionData* call, utilities::OptionData* put,
                           double entry_price, const std::string& reason);
    void enter_atm_iron_condor();
    void reset_position();

    int position_size_;
    std::vector<std::string> chain_symbols_;
    int trade_count_ = 0;
    int step_in_cycle_ = 0;
};

} // namespace strategy_cpp
