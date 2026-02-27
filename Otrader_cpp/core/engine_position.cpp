/**
 * Shared PositionEngine (from engines/engine_position.py).
 * Same logic: holdings, process_order/process_trade, apply_position_change, update_metrics.
 * Serialize/load_serialized_holding use protobuf (StrategyHoldingMsg) for binary schema.
 */

#include "engine_position.hpp"
#include "otrader_engine.pb.h"
#include <cmath>
#include <sstream>
#include <stdexcept>

namespace engines {

namespace {
/** HALF_UP rounding by decimal digits (factor-based); avoids 0.01 float representation issues. */
inline auto round_digits(double value, int digits) -> double {
    if (digits < 0) {
        return value;
    }
    const double factor = std::pow(10.0, digits);
    const double scaled = value * factor;
    const double rounded = scaled >= 0.0 ? std::floor(scaled + 0.5) : std::ceil(scaled - 0.5);
    return rounded / factor;
}

auto combo_type_from_string(const std::string& s) -> utilities::ComboType {
    if (s == "CUSTOM") {
        return utilities::ComboType::CUSTOM;
    }
    if (s == "SPREAD") {
        return utilities::ComboType::SPREAD;
    }
    if (s == "STRADDLE") {
        return utilities::ComboType::STRADDLE;
    }
    if (s == "STRANGLE") {
        return utilities::ComboType::STRANGLE;
    }
    if (s == "DIAGONAL_SPREAD") {
        return utilities::ComboType::DIAGONAL_SPREAD;
    }
    if (s == "RATIO_SPREAD") {
        return utilities::ComboType::RATIO_SPREAD;
    }
    if (s == "RISK_REVERSAL") {
        return utilities::ComboType::RISK_REVERSAL;
    }
    if (s == "BUTTERFLY") {
        return utilities::ComboType::BUTTERFLY;
    }
    if (s == "INVERSE_BUTTERFLY") {
        return utilities::ComboType::INVERSE_BUTTERFLY;
    }
    if (s == "IRON_CONDOR") {
        return utilities::ComboType::IRON_CONDOR;
    }
    if (s == "IRON_BUTTERFLY") {
        return utilities::ComboType::IRON_BUTTERFLY;
    }
    if (s == "CONDOR") {
        return utilities::ComboType::CONDOR;
    }
    if (s == "BOX_SPREAD") {
        return utilities::ComboType::BOX_SPREAD;
    }
    return utilities::ComboType::CUSTOM;
}

/** ComboType to enum name (uppercase) for JSON; matches Python ComboType.name. */
inline auto combo_type_to_enum_name(utilities::ComboType t) -> std::string {
    switch (t) {
    case utilities::ComboType::CUSTOM:
        return "CUSTOM";
    case utilities::ComboType::SPREAD:
        return "SPREAD";
    case utilities::ComboType::STRADDLE:
        return "STRADDLE";
    case utilities::ComboType::STRANGLE:
        return "STRANGLE";
    case utilities::ComboType::DIAGONAL_SPREAD:
        return "DIAGONAL_SPREAD";
    case utilities::ComboType::RATIO_SPREAD:
        return "RATIO_SPREAD";
    case utilities::ComboType::RISK_REVERSAL:
        return "RISK_REVERSAL";
    case utilities::ComboType::BUTTERFLY:
        return "BUTTERFLY";
    case utilities::ComboType::INVERSE_BUTTERFLY:
        return "INVERSE_BUTTERFLY";
    case utilities::ComboType::IRON_CONDOR:
        return "IRON_CONDOR";
    case utilities::ComboType::IRON_BUTTERFLY:
        return "IRON_BUTTERFLY";
    case utilities::ComboType::CONDOR:
        return "CONDOR";
    case utilities::ComboType::BOX_SPREAD:
        return "BOX_SPREAD";
    }
    return "CUSTOM";
}

void base_position_to_msg(const utilities::BasePosition& pos, otrader::BasePositionMsg* msg) {
    msg->set_symbol(pos.symbol);
    msg->set_quantity(pos.quantity);
    msg->set_avg_cost(pos.avg_cost);
    msg->set_cost_value(pos.cost_value);
    msg->set_realized_pnl(pos.realized_pnl);
    msg->set_mid_price(pos.mid_price);
    msg->set_delta(pos.delta);
    msg->set_gamma(pos.gamma);
    msg->set_theta(pos.theta);
    msg->set_vega(pos.vega);
}

void msg_to_base_position(const otrader::BasePositionMsg& msg, utilities::BasePosition* pos) {
    pos->symbol = msg.symbol();
    pos->quantity = msg.quantity();
    pos->avg_cost = msg.avg_cost();
    pos->cost_value = msg.cost_value();
    pos->realized_pnl = msg.realized_pnl();
    pos->mid_price = msg.mid_price();
    pos->delta = msg.delta();
    pos->gamma = msg.gamma();
    pos->theta = msg.theta();
    pos->vega = msg.vega();
}
} // namespace

void PositionEngine::process_timer_event(const GetPortfolioFn& get_portfolio,
                                         std::vector<utilities::LogData>* out_logs) {
    if (!get_portfolio) {
        return;
    }
    for (auto& kv : strategy_holdings_) {
        try {
            std::string portfolio_name = kv.first;
            size_t p = kv.first.find('_');
            if (p != std::string::npos && p + 1 < kv.first.size()) {
                portfolio_name = kv.first.substr(p + 1);
            }
            utilities::PortfolioData* portfolio = get_portfolio(portfolio_name);
            if (portfolio != nullptr) {
                update_metrics(kv.first, portfolio);
            }
        } catch (const std::exception& e) {
            if (out_logs != nullptr) {
                utilities::LogData log;
                log.msg = std::string("[PositionEngine] Metrics update error: ") + e.what();
                log.level = 0;
                log.gateway_name = "Position";
                out_logs->push_back(log);
            }
        }
    }
}

void PositionEngine::process_order(const utilities::OrderData& order) {
    OrderMeta meta;
    meta.is_combo = order.is_combo;
    meta.symbol = order.symbol;
    if (order.combo_type.has_value()) {
        meta.combo_type = utilities::to_string(order.combo_type.value());
    }
    if (order.is_combo && order.legs.has_value()) {
        for (const auto& leg : order.legs.value()) {
            std::map<std::string, std::string> m;
            m["symbol"] = leg.symbol.value_or("N/A");
            m["con_id"] = std::to_string(leg.con_id);
            m["ratio"] = std::to_string(leg.ratio);
            m["direction"] = utilities::to_string(leg.direction);
            meta.legs.push_back(m);
        }
    }
    order_meta_[order.orderid] = std::move(meta);
}

void PositionEngine::process_trade(const std::string& strategy_name,
                                   const utilities::TradeData& trade) {
    if (static_cast<unsigned int>(trade_seen_.contains(trade.tradeid)) != 0U) {
        return;
    }
    trade_seen_.insert(trade.tradeid);

    get_create_strategy_holding(strategy_name);
    utilities::StrategyHolding& holding = strategy_holdings_[strategy_name];

    auto meta_it = order_meta_.find(trade.orderid);
    if (meta_it != order_meta_.end() && meta_it->second.is_combo) {
        const OrderMeta& meta = meta_it->second;
        utilities::ComboType combo_type = meta.combo_type.has_value()
                                              ? combo_type_from_string(meta.combo_type.value())
                                              : utilities::ComboType::CUSTOM;
        utilities::ComboPositionData* combo =
            get_or_create_combo_position(holding, meta.symbol, combo_type, &meta.legs);
        if (trade.symbol == meta.symbol) {
            apply_position_change(combo, trade);
        } else {
            utilities::OptionPositionData* leg = get_or_create_option_position(*combo, trade);
            apply_position_change(static_cast<utilities::BasePosition*>(leg), trade);
        }
        return;
    }

    if (trade.symbol.size() >= 4 && trade.symbol.substr(trade.symbol.size() - 4) == ".STK") {
        apply_underlying_trade(holding, trade);
        return;
    }

    apply_single_leg_option_trade(holding, trade);
}

void PositionEngine::get_create_strategy_holding(const std::string& strategy_name) {
    if (!strategy_holdings_.contains(strategy_name)) {
        strategy_holdings_[strategy_name] = utilities::StrategyHolding();
    }
}

void PositionEngine::remove_strategy_holding(const std::string& strategy_name) {
    strategy_holdings_.erase(strategy_name);
}

auto PositionEngine::get_holding(const std::string& strategy_name) -> utilities::StrategyHolding& {
    return strategy_holdings_.at(strategy_name);
}

void PositionEngine::apply_underlying_trade(utilities::StrategyHolding& holding,
                                            const utilities::TradeData& trade) {
    utilities::UnderlyingPositionData& pos = holding.underlyingPosition;
    if (pos.symbol.empty()) {
        pos.symbol = trade.symbol;
    }
    apply_position_change(&pos, trade);
}

void PositionEngine::apply_single_leg_option_trade(utilities::StrategyHolding& holding,
                                                   const utilities::TradeData& trade) {
    auto it = holding.optionPositions.find(trade.symbol);
    if (it == holding.optionPositions.end()) {
        holding.optionPositions[trade.symbol] = utilities::OptionPositionData(trade.symbol);
        it = holding.optionPositions.find(trade.symbol);
    }
    apply_position_change(&it->second, trade);
}

auto PositionEngine::get_or_create_combo_position(
    utilities::StrategyHolding& holding, const std::string& symbol, utilities::ComboType combo_type,
    const std::vector<std::map<std::string, std::string>>* legs_meta)
    -> utilities::ComboPositionData* {
    auto it = holding.comboPositions.find(symbol);
    if (it != holding.comboPositions.end()) {
        return &it->second;
    }

    std::string norm = normalize_combo_symbol(symbol);
    for (auto& kv : holding.comboPositions) {
        if (normalize_combo_symbol(kv.first) == norm) {
            return &kv.second;
        }
    }

    holding.comboPositions[symbol] = utilities::ComboPositionData(symbol);
    utilities::ComboPositionData& combo = holding.comboPositions[symbol];
    combo.combo_type = combo_type;
    if (legs_meta != nullptr) {
        for (const auto& m : *legs_meta) {
            auto sym_it = m.find("symbol");
            combo.legs.emplace_back(sym_it != m.end() ? sym_it->second : "");
        }
    }
    return &combo;
}

auto PositionEngine::get_or_create_option_position(utilities::ComboPositionData& combo,
                                                   const utilities::TradeData& trade)
    -> utilities::OptionPositionData* {
    for (auto& leg : combo.legs) {
        if (leg.symbol == trade.symbol) {
            return &leg;
        }
    }
    combo.legs.emplace_back(trade.symbol);
    return &combo.legs.back();
}

void PositionEngine::apply_position_change(utilities::ComboPositionData* pos,
                                           const utilities::TradeData& trade) {
    int qty = static_cast<int>(std::abs(trade.volume));
    int signed_qty = (trade.direction == utilities::Direction::LONG) ? qty : -qty;
    pos->quantity += signed_qty;
    pos->cost_value = round_digits(pos->avg_cost * std::abs(pos->quantity) * pos->multiplier, 2);
}

void PositionEngine::apply_position_change(utilities::BasePosition* pos,
                                           const utilities::TradeData& trade) {
    int qty = static_cast<int>(std::abs(trade.volume));
    int signed_qty = (trade.direction == utilities::Direction::LONG) ? qty : -qty;
    int prev_qty = pos->quantity;
    double multiplier = pos->multiplier;

    if (prev_qty == 0 || (prev_qty > 0 && signed_qty > 0) || (prev_qty < 0 && signed_qty < 0)) {
        int total_qty = std::abs(prev_qty) + qty;
        if (prev_qty == 0) {
            pos->avg_cost = round_digits(trade.price, 2);
        } else {
            pos->avg_cost = round_digits(
                (pos->avg_cost * std::abs(prev_qty) + trade.price * qty) / total_qty, 2);
        }
        pos->quantity += signed_qty;
        pos->cost_value = round_digits(pos->avg_cost * std::abs(pos->quantity) * multiplier, 2);
        return;
    }

    int close_qty = std::min(std::abs(prev_qty), qty);
    double pnl = (prev_qty > 0) ? (trade.price - pos->avg_cost) * close_qty
                                : (pos->avg_cost - trade.price) * close_qty;
    pos->realized_pnl += round_digits(pnl * multiplier, 2);

    int new_qty = std::abs(prev_qty) - close_qty;
    if (new_qty == 0) {
        pos->quantity = 0;
        pos->avg_cost = 0.0;
        pos->cost_value = 0.0;
    } else {
        pos->quantity = (prev_qty > 0 ? 1 : -1) * new_qty;
        pos->cost_value = round_digits(pos->avg_cost * std::abs(pos->quantity) * multiplier, 2);
    }

    int extra = qty - close_qty;
    if (extra > 0) {
        pos->avg_cost = round_digits(trade.price, 2);
        pos->quantity = (signed_qty > 0 ? 1 : -1) * extra;
        pos->cost_value = round_digits(pos->avg_cost * std::abs(pos->quantity) * multiplier, 2);
    }
}

auto PositionEngine::accumulate_position(utilities::BasePosition* pos,
                                         const utilities::OptionData* option_snapshot)
    -> std::map<std::string, double> {
    double delta = 0;
    double gamma = 0;
    double theta = 0;
    double vega = 0;
    double mid_price = 0;
    if (option_snapshot != nullptr) {
        delta = option_snapshot->delta;
        gamma = option_snapshot->gamma;
        theta = option_snapshot->theta;
        vega = option_snapshot->vega;
        mid_price = option_snapshot->mid_price;
    }
    pos->delta = round_digits(delta, 4);
    pos->gamma = round_digits(gamma, 4);
    pos->theta = round_digits(theta, 4);
    pos->vega = round_digits(vega, 4);
    pos->mid_price = round_digits(mid_price, 2);
    std::map<std::string, double> m;
    m["cv"] = round_digits(pos->current_value(), 2);
    m["tc"] = round_digits(pos->cost_value, 2);
    m["rlz"] = round_digits(pos->realized_pnl, 2);
    m["delta"] = round_digits(pos->quantity * pos->delta, 4);
    m["gamma"] = round_digits(pos->quantity * pos->gamma, 4);
    m["theta"] = round_digits(pos->quantity * pos->theta, 4);
    m["vega"] = round_digits(pos->quantity * pos->vega, 4);
    return m;
}

auto PositionEngine::accumulate_position(utilities::BasePosition* pos,
                                         const utilities::UnderlyingData* underlying_snapshot)
    -> std::map<std::string, double> {
    double delta = 1.0;
    double mid_price = 0;
    if (underlying_snapshot != nullptr) {
        delta = underlying_snapshot->theo_delta;
        mid_price = underlying_snapshot->mid_price;
    }
    pos->delta = round_digits(delta, 4);
    pos->mid_price = round_digits(mid_price, 2);
    std::map<std::string, double> m;
    m["cv"] = round_digits(pos->current_value(), 2);
    m["tc"] = round_digits(pos->cost_value, 2);
    m["rlz"] = round_digits(pos->realized_pnl, 2);
    m["delta"] = round_digits(pos->quantity * pos->delta, 4);
    m["gamma"] = round_digits(pos->quantity * pos->gamma, 4);
    m["theta"] = round_digits(pos->quantity * pos->theta, 4);
    m["vega"] = round_digits(pos->quantity * pos->vega, 4);
    return m;
}

auto PositionEngine::accumulate_combo_position(utilities::ComboPositionData& combo,
                                               utilities::PortfolioData* portfolio)
    -> std::map<std::string, double> {
    combo.delta = combo.gamma = combo.theta = combo.vega = 0.0;
    combo.cost_value = 0.0;
    combo.realized_pnl = 0.0;
    double current_value = 0.0;

    for (auto& leg : combo.legs) {
        const utilities::OptionData* inst = nullptr;
        auto it = portfolio->options.find(leg.symbol);
        if (it != portfolio->options.end()) {
            inst = &it->second;
        }
        auto acc = accumulate_position(&leg, inst);
        current_value += acc["cv"];
        combo.cost_value += acc["tc"];
        combo.realized_pnl += acc["rlz"];
        combo.delta += acc["delta"];
        combo.gamma += acc["gamma"];
        combo.theta += acc["theta"];
        combo.vega += acc["vega"];
    }

    if (combo.quantity != 0) {
        combo.mid_price =
            round_digits(current_value / (std::abs(combo.quantity) * combo.multiplier), 2);
        if (combo.cost_value > 0) {
            combo.avg_cost =
                round_digits(combo.cost_value / (std::abs(combo.quantity) * combo.multiplier), 2);
        }
    }

    std::map<std::string, double> m;
    m["cv"] = round_digits(current_value, 2);
    m["tc"] = round_digits(combo.cost_value, 2);
    m["rlz"] = round_digits(combo.realized_pnl, 2);
    m["delta"] = round_digits(combo.delta, 4);
    m["gamma"] = round_digits(combo.gamma, 4);
    m["theta"] = round_digits(combo.theta, 4);
    m["vega"] = round_digits(combo.vega, 4);
    return m;
}

void PositionEngine::add_totals(std::map<std::string, double>& totals,
                                const std::map<std::string, double>& metrics) {
    for (auto& kv : totals) {
        kv.second += (static_cast<unsigned int>(metrics.contains(kv.first)) != 0U)
                         ? metrics.at(kv.first)
                         : 0.0;
    }
}

auto PositionEngine::normalize_combo_symbol(const std::string& symbol) -> std::string {
    // Match engines/engine_position.py: symbol.split("_", 2) -> parts[0]_parts[2] (from left).
    size_t i1 = symbol.find('_');
    if (i1 == std::string::npos) {
        return symbol;
    }
    size_t i2 = symbol.find('_', i1 + 1);
    if (i2 == std::string::npos) {
        return symbol;
    }
    return symbol.substr(0, i1) + "_" + symbol.substr(i2 + 1);
}

void PositionEngine::update_metrics(const std::string& strategy_name,
                                    utilities::PortfolioData* portfolio) {
    if (portfolio == nullptr) {
        return;
    }
    utilities::StrategyHolding& holding = strategy_holdings_.at(strategy_name);

    std::map<std::string, double> totals{{"cv", 0.0},    {"tc", 0.0},    {"rlz", 0.0},
                                         {"delta", 0.0}, {"gamma", 0.0}, {"theta", 0.0},
                                         {"vega", 0.0}};

    for (auto& kv : holding.optionPositions) {
        const utilities::OptionData* opt = nullptr;
        auto it = portfolio->options.find(kv.second.symbol);
        if (it != portfolio->options.end()) {
            opt = &it->second;
        }
        add_totals(totals, accumulate_position(&kv.second, opt));
    }

    if (holding.underlyingPosition.quantity != 0 || holding.underlyingPosition.realized_pnl != 0) {
        add_totals(totals,
                   accumulate_position(&holding.underlyingPosition, portfolio->underlying.get()));
    }

    for (auto& kv : holding.comboPositions) {
        add_totals(totals, accumulate_combo_position(kv.second, portfolio));
    }

    double unreal = totals["cv"] - totals["tc"];
    holding.summary.current_value = round_digits(totals["cv"], 2);
    holding.summary.total_cost = round_digits(totals["tc"], 2);
    holding.summary.unrealized_pnl = round_digits(unreal, 2);
    holding.summary.realized_pnl = round_digits(totals["rlz"], 2);
    holding.summary.pnl = holding.summary.unrealized_pnl + holding.summary.realized_pnl;
    holding.summary.delta = round_digits(totals["delta"], 4);
    holding.summary.gamma = round_digits(totals["gamma"], 4);
    holding.summary.theta = round_digits(totals["theta"], 4);
    holding.summary.vega = round_digits(totals["vega"], 4);

    for (auto& kv : holding.optionPositions) {
        kv.second.clear_fields();
    }
    holding.underlyingPosition.clear_fields();
    for (auto& kv : holding.comboPositions) {
        kv.second.clear_fields();
    }
}

auto PositionEngine::serialize_holding(const std::string& strategy_name) const -> std::string {
    auto it = strategy_holdings_.find(strategy_name);
    if (it == strategy_holdings_.end()) {
        return "";
    }
    const utilities::StrategyHolding& holding = it->second;
    otrader::StrategyHoldingMsg msg;
    base_position_to_msg(holding.underlyingPosition, msg.mutable_underlying());
    for (const auto& kv : holding.optionPositions) {
        base_position_to_msg(kv.second, &(*msg.mutable_options())[kv.first]);
    }
    for (const auto& kv : holding.comboPositions) {
        const utilities::ComboPositionData& c = kv.second;
        otrader::ComboPositionMsg* cm = msg.add_combos();
        cm->set_symbol(c.symbol);
        cm->set_quantity(c.quantity);
        cm->set_combo_type(combo_type_to_enum_name(c.combo_type));
        cm->set_avg_cost(c.avg_cost);
        cm->set_cost_value(c.cost_value);
        cm->set_realized_pnl(c.realized_pnl);
        cm->set_mid_price(c.mid_price);
        cm->set_delta(c.delta);
        cm->set_gamma(c.gamma);
        cm->set_theta(c.theta);
        cm->set_vega(c.vega);
        for (const auto& leg : c.legs) {
            base_position_to_msg(leg, cm->add_legs());
        }
    }
    otrader::PortfolioSummaryMsg* sm = msg.mutable_summary();
    sm->set_total_cost(holding.summary.total_cost);
    sm->set_current_value(holding.summary.current_value);
    sm->set_unrealized_pnl(holding.summary.unrealized_pnl);
    sm->set_realized_pnl(holding.summary.realized_pnl);
    sm->set_pnl(holding.summary.pnl);
    sm->set_delta(holding.summary.delta);
    sm->set_gamma(holding.summary.gamma);
    sm->set_theta(holding.summary.theta);
    sm->set_vega(holding.summary.vega);
    std::string out;
    return msg.SerializeToString(&out) ? out : "";
}

void PositionEngine::load_serialized_holding(const std::string& strategy_name,
                                             const std::string& data) {
    if (data.empty()) {
        return;
    }
    otrader::StrategyHoldingMsg msg;
    if (!msg.ParseFromString(data)) {
        return;
    }
    get_create_strategy_holding(strategy_name);
    utilities::StrategyHolding& holding = strategy_holdings_[strategy_name];

    if (msg.has_underlying()) {
        msg_to_base_position(msg.underlying(), &holding.underlyingPosition);
    }
    holding.optionPositions.clear();
    for (const auto& [sym, optMsg] : msg.options()) {
        utilities::OptionPositionData opt(sym);
        msg_to_base_position(optMsg, &opt);
        holding.optionPositions[sym] = std::move(opt);
    }
    holding.comboPositions.clear();
    for (const auto& cm : msg.combos()) {
        utilities::ComboPositionData combo(cm.symbol());
        combo.quantity = cm.quantity();
        combo.combo_type = combo_type_from_string(cm.combo_type());
        combo.avg_cost = cm.avg_cost();
        combo.cost_value = cm.cost_value();
        combo.realized_pnl = cm.realized_pnl();
        combo.mid_price = cm.mid_price();
        combo.delta = cm.delta();
        combo.gamma = cm.gamma();
        combo.theta = cm.theta();
        combo.vega = cm.vega();
        for (const auto& legMsg : cm.legs()) {
            utilities::OptionPositionData leg(legMsg.symbol());
            msg_to_base_position(legMsg, &leg);
            combo.legs.push_back(std::move(leg));
        }
        holding.comboPositions[combo.symbol] = std::move(combo);
    }
    if (msg.has_summary()) {
        const otrader::PortfolioSummaryMsg& sm = msg.summary();
        holding.summary.total_cost = sm.total_cost();
        holding.summary.current_value = sm.current_value();
        holding.summary.unrealized_pnl = sm.unrealized_pnl();
        holding.summary.realized_pnl = sm.realized_pnl();
        holding.summary.pnl = sm.pnl();
        holding.summary.delta = sm.delta();
        holding.summary.gamma = sm.gamma();
        holding.summary.theta = sm.theta();
        holding.summary.vega = sm.vega();
    }
}

} // namespace engines
