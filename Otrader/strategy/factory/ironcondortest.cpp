#include "ironcondortest.hpp"
#include "engine_option_strategy.hpp"

#include <chrono>
#include <cmath>
#include <utility>
#include <vector>

namespace strategy_cpp {

IronCondorTestStrategy::IronCondorTestStrategy(
    core::OptionStrategyEngine* strategy_engine, const std::string& strategy_name,
    const std::string& portfolio_name, const std::unordered_map<std::string, double>& setting)
    : OptionStrategyTemplate(strategy_engine, strategy_name, portfolio_name, setting),
      position_size_(1) {
    if (auto it = setting.find("position_size"); it != setting.end()) {
        position_size_ = static_cast<int>(it->second);
    }
}

void IronCondorTestStrategy::on_init_logic() {
    // 7DTE chain for minimal test
    auto chains = portfolio()->get_chain_by_expiry(7, 7);
    if (chains.empty()) {
        set_error("No chains found");
        return;
    }
    chain_symbols_ = {chains.front()};
    subscribe_chains(chain_symbols_);
    write_log("IronCondorTest initialized on chain: " + chains.front());
}

void IronCondorTestStrategy::on_stop_logic() {
    close_all_strategy_positions();
    write_log("Strategy stopped. Total trades: " + std::to_string(trade_count_));
}

void IronCondorTestStrategy::on_timer_logic() {
    if (error()) {
        return;
    }
    auto* h = holding();
    if (h == nullptr) {
        return;
    }

    // 3-step cycle: entry-hold-exit
    step_in_cycle_ = (step_in_cycle_ + 1) % 3;

    bool has_position = false;
    for (const auto& [_, pos] : h->optionPositions) {
        if (pos.quantity != 0) {
            has_position = true;
            break;
        }
    }

    if (step_in_cycle_ == 0) {
        // Step 3: close
        if (has_position) {
            close_all_strategy_positions();
            reset_position();
        }
        return;
    }

    if (step_in_cycle_ == 1) {
        // Step 1: open
        if (!has_position) {
            enter_atm_iron_condor();
        }
    }
    // Step 2: hold
}

void IronCondorTestStrategy::enter_iron_condor(utilities::OptionData* call,
                                               utilities::OptionData* put, double entry_price,
                                               const std::string& reason) {
    if ((call == nullptr) || (put == nullptr)) {
        return;
    }
    // Iron condor: K-Δ, K, K, K+Δ
    auto* chain = call->chain;
    if ((chain == nullptr) || chain != put->chain) {
        return;
    }
    const auto& idxs = chain->indexes;
    if (idxs.empty() || chain->atm_index.empty()) {
        return;
    }

    // ATM index
    int atm_pos = -1;
    for (int i = 0; i < static_cast<int>(idxs.size()); ++i) {
        if (idxs[static_cast<size_t>(i)] == chain->atm_index) {
            atm_pos = i;
            break;
        }
    }
    if (atm_pos < 0) {
        return;
    }

    // Wings: 1 step
    constexpr int kWingSteps = 1;
    const size_t lower_pos = (atm_pos >= kWingSteps) ? static_cast<size_t>(atm_pos - kWingSteps)
                                                     : idxs.size(); // out of range
    const size_t upper_pos = static_cast<size_t>(atm_pos) + static_cast<size_t>(kWingSteps);
    if (lower_pos >= idxs.size() || upper_pos >= idxs.size()) {
        return;
    }

    const std::string& lower_idx = idxs[lower_pos];
    const std::string& upper_idx = idxs[upper_pos];

    auto pl_it = chain->puts.find(lower_idx);
    auto pu_it = chain->puts.find(chain->atm_index);
    auto cl_it = chain->calls.find(chain->atm_index);
    auto cu_it = chain->calls.find(upper_idx);
    if (pl_it == chain->puts.end() || pu_it == chain->puts.end() || cl_it == chain->calls.end() ||
        cu_it == chain->calls.end()) {
        return;
    }
    auto* put_lower = pl_it->second;
    auto* put_body = pu_it->second;
    auto* call_body = cl_it->second;
    auto* call_upper = cu_it->second;
    if ((put_lower == nullptr) || (put_body == nullptr) || (call_body == nullptr) ||
        (call_upper == nullptr)) {
        return;
    }
    if (put_lower->mid_price <= 0.0 || put_body->mid_price <= 0.0 || call_body->mid_price <= 0.0 ||
        call_upper->mid_price <= 0.0) {
        return;
    }

    std::unordered_map<std::string, utilities::OptionData*> option_data;
    option_data["put_lower"] = put_lower;
    option_data["put_upper"] = put_body;
    option_data["call_lower"] = call_body;
    option_data["call_upper"] = call_upper;

    // SHORT iron condor
    auto ids =
        option_order(utilities::ComboType::IRON_CONDOR, option_data, utilities::Direction::SHORT,
                     0.0, static_cast<double>(position_size_), utilities::OrderType::MARKET);
    if (!ids.empty()) {
        trade_count_++;
        write_log("Entered IronCondor @" + std::to_string(entry_price) + " reason=" + reason);
    }
}

void IronCondorTestStrategy::enter_atm_iron_condor() {
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
    enter_iron_condor(call, put, total_price, "loop_atm_iron_condor");
}

void IronCondorTestStrategy::reset_position() {
    // No extra state to reset
}

} // namespace strategy_cpp
