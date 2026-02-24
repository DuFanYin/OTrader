#include "template.hpp"
#include "../core/engine_combo_builder.hpp"
#include "../core/engine_hedge.hpp"
#include "../core/engine_option_strategy.hpp"

namespace strategy_cpp {

OptionStrategyTemplate::OptionStrategyTemplate(
    core::OptionStrategyEngine* strategy_engine,
    std::string strategy_name,
    std::string portfolio_name,
    const std::unordered_map<std::string, double>& setting
)
    : engine_(strategy_engine),
      strategy_name_(std::move(strategy_name)),
      portfolio_name_(std::move(portfolio_name)) {
    if (!engine_) {
        throw std::runtime_error("Strategy engine is null");
    }
    portfolio_ = engine_->get_portfolio(portfolio_name_);
    if (!portfolio_) {
        throw std::runtime_error("Portfolio not found: " + portfolio_name_);
    }
    underlying_ = portfolio_->underlying.get();
    auto it = setting.find("timer_trigger");
    if (it != setting.end()) {
        timer_trigger_ = static_cast<int>(it->second);
    }
    write_log("Strategy " + strategy_name_ + " created for portfolio " + portfolio_name_);
}

void OptionStrategyTemplate::on_init() {
    inited_ = true;
    on_init_logic();
}

void OptionStrategyTemplate::on_start() {
    started_ = true;
}

void OptionStrategyTemplate::on_stop() {
    started_ = false;
    on_stop_logic();
}

void OptionStrategyTemplate::on_timer() {
    if (!started_ || error_) {
        return;
    }
    timer_cnt_++;
    if (timer_cnt_ >= timer_trigger_) {
        timer_cnt_ = 0;
        on_timer_logic();
    }
}

void OptionStrategyTemplate::on_order(const utilities::OrderData& order) {
    std::string dir = order.direction ? utilities::to_string(*order.direction) : "";
    write_log("Order " + order.orderid + ": " + dir + " " + std::to_string(order.volume) + " @ " +
              std::to_string(order.price) + " [" + utilities::to_string(order.status) + "]");
}

void OptionStrategyTemplate::on_trade(const utilities::TradeData& trade) {
    std::string dir = trade.direction ? utilities::to_string(*trade.direction) : "";
    write_log("Trade " + trade.tradeid + ": " + dir + " " + std::to_string(trade.volume) + " @ " +
              std::to_string(trade.price));
}

void OptionStrategyTemplate::subscribe_chains(const std::vector<std::string>& chain_symbols) {
    chain_map_.clear();
    for (const auto& sym : chain_symbols) {
        auto it = portfolio_->chains.find(sym);
        if (it != portfolio_->chains.end() && it->second) {
            chain_map_[sym] = it->second.get();
        }
    }
}

utilities::ChainData* OptionStrategyTemplate::get_chain(const std::string& chain_symbol) const {
    auto it = chain_map_.find(chain_symbol);
    return it == chain_map_.end() ? nullptr : it->second;
}

std::vector<std::string> OptionStrategyTemplate::underlying_order(
    utilities::Direction direction, double price, double volume, utilities::OrderType order_type
) {
    if (!underlying_) {
        return {};
    }
    return engine_->send_order(strategy_name_, underlying_->symbol, direction, price, volume, order_type);
}

std::vector<std::string> OptionStrategyTemplate::option_order(
    const utilities::OptionData& option_data, utilities::Direction direction, double price, double volume, utilities::OrderType order_type
) {
    return engine_->send_order(strategy_name_, option_data.symbol, direction, price, volume, order_type);
}

std::vector<std::string> OptionStrategyTemplate::combo_order(
    utilities::ComboType combo_type,
    const std::unordered_map<std::string, utilities::OptionData*>& option_data,
    utilities::Direction direction, double price, double volume, utilities::OrderType order_type
) {
    auto* cb = engine_->combo_builder_engine();
    if (!cb) return {};
    auto get_contract = [this](const std::string& s) { return engine_->get_contract(s); };
    std::vector<utilities::LogData> combo_logs;
    auto [legs, sig] = cb->combo_builder(option_data, combo_type, direction, static_cast<int>(volume), get_contract, &combo_logs);
    for (const auto& l : combo_logs)
        engine_->write_log(l.msg, l.level);
    return engine_->send_combo_order(strategy_name_, combo_type, sig, direction, price, volume, legs, order_type);
}

void OptionStrategyTemplate::register_hedging(int timer_trigger, int delta_target, int delta_range) {
    if (!engine_) return;
    auto* hedge = engine_->hedge_engine();
    if (!hedge) return;
    hedge->register_strategy(strategy_name(), timer_trigger, delta_target, delta_range);
}

void OptionStrategyTemplate::unregister_hedging() {
    if (!engine_) return;
    auto* hedge = engine_->hedge_engine();
    if (!hedge) return;
    hedge->unregister_strategy(strategy_name_);
}

void OptionStrategyTemplate::close_all_strategy_positions() {
    if (!holding_) return;
    for (const auto& [_, combo] : holding_->comboPositions) {
        if (combo.quantity == 0) continue;
        std::unordered_map<std::string, utilities::OptionData*> option_data;
        for (const auto& leg : combo.legs) {
            auto it = portfolio_->options.find(leg.symbol);
            if (it != portfolio_->options.end()) option_data[leg.symbol] = &it->second;
        }
        if (option_data.empty()) continue;
        utilities::Direction dir = combo.quantity > 0 ? utilities::Direction::SHORT : utilities::Direction::LONG;
        combo_order(
            utilities::ComboType::CUSTOM,
            option_data,
            dir,
            0.0,
            std::abs(static_cast<double>(combo.quantity)),
            utilities::OrderType::MARKET
        );
    }
    for (const auto& [sym, pos] : holding_->optionPositions) {
        if (pos.quantity == 0) continue;
        utilities::Direction dir = pos.quantity > 0 ? utilities::Direction::SHORT : utilities::Direction::LONG;
        engine_->send_order(strategy_name_, sym, dir, 0.0, std::abs(static_cast<double>(pos.quantity)), utilities::OrderType::MARKET);
    }
    if (holding_->underlyingPosition.quantity != 0 && underlying_) {
        utilities::Direction dir = holding_->underlyingPosition.quantity > 0 ? utilities::Direction::SHORT : utilities::Direction::LONG;
        engine_->send_order(strategy_name_, underlying_->symbol, dir, 0.0, std::abs(static_cast<double>(holding_->underlyingPosition.quantity)), utilities::OrderType::MARKET);
    }
}

void OptionStrategyTemplate::set_error(const std::string& msg) {
    error_ = true;
    error_msg_ = msg;
    started_ = false;
    write_log("ERROR: " + msg);
}

void OptionStrategyTemplate::write_log(const std::string& msg) const {
    if (engine_) engine_->write_log("[" + strategy_name_ + "] " + msg, 20 /* INFO */);
}

}  // namespace strategy_cpp
