#include "template.hpp"
#include <array>
#include <cmath>
#include <stdexcept>

namespace strategy {

namespace {

// name, default, min, max, integer
constexpr std::array kBaseParameters{
    StrategyParam{"timer_trigger", 1, 1, 390, true}, // run on_timer_logic every N bars
};

} // namespace

OptionStrategyTemplate::OptionStrategyTemplate(
    StrategyContext* context, std::string strategy_name, std::string portfolio_name,
    const std::unordered_map<std::string, double>& setting)
    : context_(context), strategy_name_(std::move(strategy_name)),
      portfolio_name_(std::move(portfolio_name)), setting_(setting) {
    if (context_ == nullptr) {
        throw std::runtime_error("Strategy context is null");
    }
    portfolio_ = context_->get_portfolio(portfolio_name_);
    if (portfolio_ == nullptr) {
        throw std::runtime_error("Portfolio not found: " + portfolio_name_);
    }
    underlying_ = portfolio_->underlying.get();
    timer_trigger_ = param<int>("timer_trigger");
    write_log("Strategy " + strategy_name_ + " created for portfolio " + portfolio_name_);
}

auto OptionStrategyTemplate::base_parameters() -> std::span<const StrategyParam> {
    return kBaseParameters;
}

auto OptionStrategyTemplate::param_value(const std::string& name) const -> double {
    auto it = setting_.find(name);
    if (it == setting_.end()) {
        throw std::logic_error(strategy_name_ + " reads parameter '" + name +
                               "', which its class does not declare");
    }
    return it->second;
}

void OptionStrategyTemplate::on_init() {
    inited_ = true;
    on_init_logic();
}

void OptionStrategyTemplate::on_start() { started_ = true; }

void OptionStrategyTemplate::on_stop() {
    started_ = false;
    on_stop_logic();
}

void OptionStrategyTemplate::on_timer() {
    if (!started_ || error_ || !bar_gate_.next(now())) {
        return;
    }
    timer_cnt_++;
    if (timer_cnt_ >= timer_trigger_) {
        timer_cnt_ = 0;
        on_timer_logic();
    }
}

void OptionStrategyTemplate::on_order(const model::OrderData& order) {
    std::string dir = model::to_string(order.direction);
    write_log("Order " + order.orderid + ": " + dir + " " + std::to_string(order.volume) + " @ " +
              std::to_string(order.price) + " [" + model::to_string(order.status) + "]");
}

void OptionStrategyTemplate::on_trade(const model::TradeData& trade) {
    std::string dir = model::to_string(trade.direction);
    write_log("Trade " + trade.tradeid + ": " + dir + " " + std::to_string(trade.volume) + " @ " +
              std::to_string(trade.price));
}

void OptionStrategyTemplate::subscribe_chains(std::span<const std::string> chain_symbols) {
    chain_map_.clear();
    for (const auto& sym : chain_symbols) {
        auto it = portfolio_->chains.find(sym);
        if (it != portfolio_->chains.end() && it->second) {
            chain_map_[sym] = it->second.get();
        }
    }
    context_->subscribe_chains(strategy_name_, chain_symbols);
}

auto OptionStrategyTemplate::get_chain(const std::string& chain_symbol) const
    -> const model::ChainData* {
    auto it = chain_map_.find(chain_symbol);
    return it == chain_map_.end() ? nullptr : it->second;
}

auto OptionStrategyTemplate::underlying_order(model::Direction direction, double price,
                                              double volume, model::OrderType order_type)
    -> std::vector<std::string> {
    if (underlying_ == nullptr) {
        return {};
    }
    return context_->send_order(strategy_name_, underlying_->symbol, direction, price, volume,
                                order_type);
}

auto OptionStrategyTemplate::option_order(
    model::ComboType combo_type,
    const std::unordered_map<std::string, const model::OptionData*>& option_data,
    model::Direction direction, double price, double volume, model::OrderType order_type)
    -> std::vector<std::string> {
    // One option is that option's order, not a combo of one: held as itself, and a venue need
    // know no combo for it.
    if (combo_type == model::ComboType::SINGLE_LEG && option_data.size() == 1 &&
        option_data.begin()->second != nullptr) {
        return context_->send_order(strategy_name_, option_data.begin()->second->symbol, direction,
                                    price, volume, order_type);
    }
    try {
        auto [legs, sig] =
            model::combo::build_combo(option_data, combo_type, direction, combo_options());
        return context_->send_combo_order(strategy_name_, combo_type, sig, direction, price, volume,
                                          legs, order_type);
    } catch (const std::exception& e) {
        write_log(std::string("Combo not sent: ") + e.what(), utilities::ERROR);
        return {};
    }
}

auto OptionStrategyTemplate::combo_leg(const model::OptionData& option,
                                       model::Direction direction, int ratio) const
    -> model::Leg {
    return model::combo::create_leg(option, direction, ratio, combo_options().get_contract);
}

auto OptionStrategyTemplate::custom_combo_order(std::span<const model::Leg> legs, double price,
                                                double volume, model::OrderType order_type)
    -> std::vector<std::string> {
    if (legs.empty()) {
        return {};
    }
    if (legs.size() == 1 && legs.front().ratio == 1) {
        return context_->send_order(strategy_name_, legs.front().symbol, legs.front().direction,
                                    price, volume, order_type);
    }
    // The order buys the combo; each leg carries its own side.
    return context_->send_combo_order(strategy_name_, model::ComboType::CUSTOM,
                                      model::combo::generate_combo_signature(legs),
                                      model::Direction::LONG, price, volume, legs, order_type);
}

void OptionStrategyTemplate::register_hedging(int timer_trigger, int delta_target,
                                              int delta_range) {
    context_->register_hedging(strategy_name_, portfolio_name_, timer_trigger, delta_target,
                               delta_range);
}

void OptionStrategyTemplate::unregister_hedging() { context_->unregister_hedging(strategy_name_); }

void OptionStrategyTemplate::close_all_strategy_positions() {
    if (holding_ == nullptr) {
        return;
    }
    for (const auto& [_, pos] : holding_->optionPositions) {
        close_strategy_position(pos);
    }
    // Shares: of whatever they are in (the hedge instrument, SPY for SPXW, or the underlying).
    const model::UnderlyingPositionData& shares = holding_->underlyingPosition;
    if (shares.quantity != 0 && !shares.symbol.empty()) {
        const model::Direction dir =
            shares.quantity > 0 ? model::Direction::SHORT : model::Direction::LONG;
        context_->send_order(strategy_name_, shares.symbol, dir, 0.0,
                             std::abs(static_cast<double>(shares.quantity)),
                             model::OrderType::MARKET);
    }
}

void OptionStrategyTemplate::close_strategy_position(const model::OptionPositionData& pos) {
    if (pos.quantity == 0) {
        return;
    }
    std::unordered_map<std::string, const model::OptionData*> option_data;
    if (!pos.legs.empty()) {
        for (const auto& leg : pos.legs) {
            auto it = portfolio_->options.find(leg.symbol);
            if (it != portfolio_->options.end()) {
                option_data[leg.symbol] = &it->second;
            }
        }
    } else {
        auto it = portfolio_->options.find(pos.symbol);
        if (it != portfolio_->options.end()) {
            option_data[pos.symbol] = &it->second;
        }
    }
    if (option_data.empty()) {
        return;
    }
    model::Direction dir =
        pos.quantity > 0 ? model::Direction::SHORT : model::Direction::LONG;
    const double volume = std::abs(static_cast<double>(pos.quantity));
    if (pos.legs.empty()) {
        option_order(model::ComboType::SINGLE_LEG, option_data, dir, 0.0, volume,
                     model::OrderType::MARKET);
        return;
    }
    // A combo: one order over its open legs, each against its own side; the position engine
    // books its fills to this combo (the one holding those legs).
    try {
        auto [legs, sig] =
            model::combo::build_closing_combo(pos, option_data, combo_options());
        context_->send_combo_order(strategy_name_, model::ComboType::CUSTOM, sig, dir, 0.0,
                                   volume, legs, model::OrderType::MARKET);
    } catch (const std::exception& e) {
        write_log(std::string("Closing combo not sent: ") + e.what(), utilities::ERROR);
    }
}

auto OptionStrategyTemplate::has_active_order() const -> bool {
    return context_->has_active_order(strategy_name_);
}

auto OptionStrategyTemplate::combo_options() const -> model::combo::ComboBuildOptions {
    model::combo::ComboBuildOptions options;
    options.get_contract = [this](const std::string& symbol) -> const model::ContractData* {
        return context_->get_contract(symbol);
    };
    return options;
}

void OptionStrategyTemplate::set_error(const std::string& msg) {
    error_ = true;
    error_msg_ = msg;
    started_ = false;
    write_log(msg, utilities::ERROR);
}

void OptionStrategyTemplate::write_log(const std::string& msg, utilities::LogLevel level) const {
    context_->write_log(strategy_name_, msg, level);
}

} // namespace strategy
