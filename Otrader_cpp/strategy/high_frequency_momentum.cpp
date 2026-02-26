#include "high_frequency_momentum.hpp"
#include "engine_option_strategy.hpp"

#include <chrono>
#include <cmath>
#include <vector>

namespace strategy_cpp {

HighFrequencyMomentumStrategy::HighFrequencyMomentumStrategy(
    core::OptionStrategyEngine* strategy_engine, const std::string& strategy_name,
    const std::string& portfolio_name, const std::unordered_map<std::string, double>& setting)
    : OptionStrategyTemplate(strategy_engine, strategy_name, portfolio_name, setting) {
    if (auto it = setting.find("position_size"); it != setting.end())
        position_size_ = static_cast<int>(it->second);
}

void HighFrequencyMomentumStrategy::on_init_logic() {
    // 只选 7DTE（约一周后到期）链，用来做极简测试策略
    auto chains = portfolio()->get_chain_by_expiry(7, 7);
    if (chains.empty()) {
        set_error("No chains found");
        return;
    }
    chain_symbols_ = {chains.front()};
    subscribe_chains(chain_symbols_);
    write_log("HighFrequencyMomentum initialized on chain: " + chains.front());
}

void HighFrequencyMomentumStrategy::on_stop_logic() {
    close_all_strategy_positions();
    write_log("Strategy stopped. Total trades: " + std::to_string(trade_count_));
}

void HighFrequencyMomentumStrategy::on_timer_logic() {
    if (error())
        return;
    auto* h = holding();
    if (!h)
        return;

    // 简化策略：每 3 个 timer 完成一次「开仓-持有-平仓」循环。
    // step_in_cycle_: 0 → 1 → 2 → 0 → ...
    step_in_cycle_ = (step_in_cycle_ + 1) % 3;

    bool has_position = false;
    for (const auto& [_, pos] : h->optionPositions) {
        if (pos.quantity != 0) {
            has_position = true;
            break;
        }
    }
    if (!has_position) {
        for (const auto& [_, combo_pos] : h->comboPositions) {
            if (combo_pos.quantity != 0) {
                has_position = true;
                break;
            }
        }
    }

    if (step_in_cycle_ == 0) {
        // 第 3 步：如果有仓位，就平仓
        if (has_position) {
            close_all_strategy_positions();
            reset_position();
        }
        return;
    }

    if (step_in_cycle_ == 1) {
        // 第 1 步：如果当前无仓位，则开一手 ATM straddle
        if (!has_position) {
            enter_atm_straddle();
        }
    }
    // 第 2 步：仅持有，不操作
}

void HighFrequencyMomentumStrategy::enter_straddle(utilities::OptionData* call,
                                                   utilities::OptionData* put, double entry_price,
                                                   const std::string& reason) {
    if (!call || !put)
        return;
    std::unordered_map<std::string, utilities::OptionData*> option_data;
    option_data["call"] = call;
    option_data["put"] = put;
    auto ids = combo_order(utilities::ComboType::STRADDLE, option_data, utilities::Direction::LONG,
                           0.0, static_cast<double>(position_size_), utilities::OrderType::MARKET);
    if (!ids.empty()) {
        trade_count_++;
        write_log("Entered STRADDLE @" + std::to_string(entry_price) + " reason=" + reason);
    }
}

void HighFrequencyMomentumStrategy::enter_atm_straddle() {
    if (chain_symbols_.empty())
        return;
    auto* chain = get_chain(chain_symbols_.front());
    if (!chain)
        return;
    chain->calculate_atm_price();
    if (chain->atm_index.empty())
        return;

    auto c_it = chain->calls.find(chain->atm_index);
    auto p_it = chain->puts.find(chain->atm_index);
    if (c_it == chain->calls.end() || p_it == chain->puts.end())
        return;
    auto* call = c_it->second;
    auto* put = p_it->second;
    if (!call || !put)
        return;
    if (call->mid_price <= 0.0 || put->mid_price <= 0.0)
        return;

    double total_price = call->mid_price + put->mid_price;
    enter_straddle(call, put, total_price, "loop_atm_straddle");
}

void HighFrequencyMomentumStrategy::reset_position() {
    // 当前简化策略没有额外需要重置的内部状态
}

} // namespace strategy_cpp
