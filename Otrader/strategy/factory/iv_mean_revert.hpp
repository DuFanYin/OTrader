#pragma once

#include "../core/engine_option_strategy.hpp"
#include "template.hpp"

#include <string>
#include <unordered_map>
#include <vector>

namespace strategy_cpp {

// IV mean regression strategy
class IvMeanRevertStrategy final : public OptionStrategyTemplate {
  public:
    IvMeanRevertStrategy(core::OptionStrategyEngine* strategy_engine,
                         const std::string& strategy_name, const std::string& portfolio_name,
                         const std::unordered_map<std::string, double>& setting);

    void on_init_logic() override;
    void on_stop_logic() override;
    void on_timer_logic() override;

  private:
    double iv_threshold_up_;
    double iv_threshold_down_;
    int lookback_minutes_;
    int position_size_;
    int min_dte_;
    int max_dte_;
    double exit_z_;
    int warmup_bars_;

    std::vector<std::string> chain_symbols_;
    std::vector<double> iv_history_;
    double last_z_ = 0.0;
};

} // namespace strategy_cpp
