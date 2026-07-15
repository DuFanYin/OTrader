#include "straddletest.hpp"
#include "engine_option_strategy.hpp"

#include <chrono>
#include <cmath>
#include <vector>

namespace strategy_cpp {

StraddleTestStrategy::StraddleTestStrategy(core::OptionStrategyEngine* strategy_engine,
                                           const std::string& strategy_name,
                                           const std::string& portfolio_name,
                                           const std::unordered_map<std::string, double>& setting)
    : OptionStrategyTemplate(strategy_engine, strategy_name, portfolio_name, setting),
      position_size_(1) {
    if (auto it = setting.find("position_size"); it != setting.end()) {
        position_size_ = static_cast<int>(it->second);
    }
}

void StraddleTestStrategy::on_init_logic() {
    // 7DTE chain
    auto chains = portfolio()->get_chain_by_expiry(7, 7);
    if (chains.empty()) {
        set_error("No chains found");
        return;
    }
    chain_symbols_ = {chains.front()};
    subscribe_chains(chain_symbols_);
    write_log("StraddleTest initialized on chain: " + chains.front());
}

void StraddleTestStrategy::on_stop_logic() {
    write_log("Strategy stopped. Total trades: " + std::to_string(trade_count_));
}

void StraddleTestStrategy::on_timer_logic() {
    if (error()) {
        return;
    }
    auto* h = holding();
    if (h == nullptr) {
        return;
    }

    bool has_position = false;
    for (const auto& [_, pos] : h->optionPositions) {
        if (pos.quantity != 0) {
            has_position = true;
            break;
        }
    }

    // Single entry: no position, no active order, trade_count_==0
    bool has_active_order = false;
    if (engine_ != nullptr) {
        const auto& by_strategy = engine_->get_strategy_active_orders();
        auto it = by_strategy.find(strategy_name());
        if (it != by_strategy.end()) {
            for (const auto& oid : it->second) {
                if (auto* o = engine_->get_order(oid); o != nullptr && o->is_active()) {
                    has_active_order = true;
                    break;
                }
            }
        }
    }

    if (!has_position && !has_active_order && trade_count_ == 0) {
        enter_atm_straddle();
    }
}

void StraddleTestStrategy::enter_straddle(utilities::OptionData* call, utilities::OptionData* put,
                                          double entry_price, const std::string& reason) {
    if ((call == nullptr) || (put == nullptr)) {
        return;
    }
    std::unordered_map<std::string, utilities::OptionData*> option_data;
    option_data["call"] = call;
    option_data["put"] = put;
    auto ids = option_order(utilities::ComboType::STRADDLE, option_data, utilities::Direction::LONG,
                            0.0, static_cast<double>(position_size_), utilities::OrderType::MARKET);
    if (!ids.empty()) {
        trade_count_++;
        write_log("Entered STRADDLE @" + std::to_string(entry_price) + " reason=" + reason);
    }
}

void StraddleTestStrategy::enter_atm_straddle() {
    if (chain_symbols_.empty()) {
        return;
    }
    auto* chain = get_chain(chain_symbols_.front());
    if (chain == nullptr) {
        return;
    }
    chain->calculate_atm_price();
    if (chain->atm_index.empty()) {
        return;
    }

    auto c_it = chain->calls.find(chain->atm_index);
    auto p_it = chain->puts.find(chain->atm_index);
    if (c_it == chain->calls.end() || p_it == chain->puts.end()) {
        return;
    }
    auto* call = c_it->second;
    auto* put = p_it->second;
    if ((call == nullptr) || (put == nullptr)) {
        return;
    }
    if (call->mid_price <= 0.0 || put->mid_price <= 0.0) {
        return;
    }

    double total_price = call->mid_price + put->mid_price;
    enter_straddle(call, put, total_price, "loop_atm_straddle");
}

void StraddleTestStrategy::reset_position() {
    // No extra state
}

} // namespace strategy_cpp
