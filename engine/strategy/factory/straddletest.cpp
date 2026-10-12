#include "straddletest.hpp"
#include "strategy_registry.hpp"

#include <array>
#include <chrono>
#include <cmath>
#include <vector>

namespace strategy {

namespace {

// name, default, min, max, integer
constexpr std::array kParameters{
    StrategyParam{"position_size", 1, 0, 100, true}, // 0 sends empty orders: drives the engine
};

} // namespace

StraddleTestStrategy::StraddleTestStrategy(StrategyContext* context,
                                           const std::string& strategy_name,
                                           const std::string& portfolio_name,
                                           const std::unordered_map<std::string, double>& setting)
    : OptionStrategyTemplate(context, strategy_name, portfolio_name, setting),
      position_size_(param<int>("position_size")) {}

auto StraddleTestStrategy::parameters() -> std::span<const StrategyParam> { return kParameters; }

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
    if (!has_position && !has_active_order() && trade_count_ == 0) {
        enter_atm_straddle();
    }
}

void StraddleTestStrategy::enter_straddle(const model::OptionData* call,
                                          const model::OptionData* put, double entry_price,
                                          const std::string& reason) {
    if ((call == nullptr) || (put == nullptr)) {
        return;
    }
    std::unordered_map<std::string, const model::OptionData*> option_data;
    option_data["call"] = call;
    option_data["put"] = put;
    auto ids = option_order(model::ComboType::STRADDLE, option_data, model::Direction::LONG,
                            0.0, static_cast<double>(position_size_), model::OrderType::MARKET);
    if (!ids.empty()) {
        trade_count_++;
        write_log("Entered STRADDLE @" + std::to_string(entry_price) + " reason=" + reason);
    }
}

void StraddleTestStrategy::enter_atm_straddle() {
    if (chain_symbols_.empty()) {
        return;
    }
    const auto* chain = get_chain(chain_symbols_.front());
    if (chain == nullptr) {
        return;
    }
    if (chain->atm_strike <= 0) {
        return;
    }

    auto c_it = chain->calls.find(chain->atm_strike);
    auto p_it = chain->puts.find(chain->atm_strike);
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

REGISTER_STRATEGY(StraddleTestStrategy);

} // namespace strategy
