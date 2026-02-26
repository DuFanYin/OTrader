#include "engine_option_strategy.hpp"
#include "../strategy/strategy_registry.hpp"
#include "../strategy/template.hpp"
#include "../utilities/event.hpp"
#include "../utilities/utility.hpp"
#include <sstream>
#include <stdexcept>

namespace {

auto extract_class_name(const std::string& strategy_name) -> std::string {
    std::size_t pos = strategy_name.find('_');
    if (pos == std::string::npos) {
        return strategy_name;
    }
    return strategy_name.substr(0, pos);
}

auto extract_portfolio_name(const std::string& strategy_name) -> std::string {
    std::size_t pos = strategy_name.find('_');
    if (pos == std::string::npos || pos + 1 >= strategy_name.size()) {
        return strategy_name;
    }
    return strategy_name.substr(pos + 1);
}

} // namespace

namespace core {

OptionStrategyEngine::OptionStrategyEngine(RuntimeAPI api) : api_(std::move(api)) {}

OptionStrategyEngine::~OptionStrategyEngine() = default;

void OptionStrategyEngine::process_order(utilities::OrderData& order) {
    orders_[order.orderid] = order;
    auto it = orderid_strategy_name_.find(order.orderid);
    if (it != orderid_strategy_name_.end()) {
        std::string strategy_name = it->second;
        auto* s = get_strategy(strategy_name);
        if (s != nullptr) {
            s->on_order(order);
        }
        if (order.status == utilities::Status::CANCELLED ||
            order.status == utilities::Status::REJECTED ||
            order.status == utilities::Status::ALLTRADED) {
            strategy_active_orders_[strategy_name].erase(order.orderid);
            orderid_strategy_name_.erase(it);
            all_active_order_ids_.erase(order.orderid);
        } else if (order.status == utilities::Status::ALLTRADED) {
            strategy_active_orders_[strategy_name].erase(order.orderid);
        }
    }
}

void OptionStrategyEngine::process_trade(const utilities::TradeData& trade) {
    trades_[trade.tradeid] = trade;
    auto it = orderid_strategy_name_.find(trade.orderid);
    if (it != orderid_strategy_name_.end()) {
        auto* s = get_strategy(it->second);
        if (s != nullptr) {
            s->on_trade(trade);
        }
    }
}

auto OptionStrategyEngine::get_strategy(const std::string& strategy_name)
    -> strategy_cpp::OptionStrategyTemplate* {
    auto it = strategies_.find(strategy_name);
    return (it != strategies_.end()) ? it->second.get() : nullptr;
}

auto OptionStrategyEngine::get_strategy() -> strategy_cpp::OptionStrategyTemplate* {
    if (strategies_.size() != 1) {
        return nullptr;
    }
    return strategies_.begin()->second.get();
}

auto OptionStrategyEngine::get_strategy_holding(const std::string& strategy_name) const
    -> utilities::StrategyHolding* {
    if (!api_.get_holding) {
        return nullptr;
    }
    return api_.get_holding(strategy_name);
}

auto OptionStrategyEngine::get_strategy_holding() const -> utilities::StrategyHolding* {
    if (strategies_.size() != 1) {
        return nullptr;
    }
    return get_strategy_holding(strategies_.begin()->first);
}

auto OptionStrategyEngine::get_portfolio(const std::string& portfolio_name) const
    -> utilities::PortfolioData* {
    return api_.get_portfolio ? api_.get_portfolio(portfolio_name) : nullptr;
}

auto OptionStrategyEngine::get_holding(const std::string& strategy_name) const
    -> utilities::StrategyHolding* {
    return api_.get_holding ? api_.get_holding(strategy_name) : nullptr;
}

auto OptionStrategyEngine::get_contract(const std::string& symbol) const
    -> const utilities::ContractData* {
    return api_.get_contract ? api_.get_contract(symbol) : nullptr;
}

void OptionStrategyEngine::write_log(const std::string& msg, int level) const {
    if (api_.write_log) {
        utilities::LogData log;
        log.msg = msg;
        log.level = level;
        log.gateway_name = "Strategy";
        api_.write_log(log);
    }
}

void OptionStrategyEngine::write_log(const utilities::LogData& log) const {
    if (api_.write_log) {
        api_.write_log(log);
    }
}

auto OptionStrategyEngine::send_order(const std::string& strategy_name,
                                      const utilities::OrderRequest& req) -> std::string {
    if (!api_.send_order) {
        return {};
    }
    std::string orderid = api_.send_order(strategy_name, req);
    if (!orderid.empty()) {
        strategy_active_orders_[strategy_name].insert(orderid);
        orderid_strategy_name_[orderid] = strategy_name;
        all_active_order_ids_.insert(orderid);
    }
    return orderid;
}

auto OptionStrategyEngine::send_order(const std::string& strategy_name, const std::string& symbol,
                                      utilities::Direction direction, double price, double volume,
                                      utilities::OrderType order_type) -> std::vector<std::string> {
    const utilities::ContractData* contract = get_contract(symbol);
    if (contract == nullptr) {
        return {};
    }
    utilities::OrderRequest req;
    req.symbol = contract->symbol;
    req.exchange = contract->exchange;
    req.direction = direction;
    req.type = order_type;
    req.volume = utilities::round_to(volume, contract->min_volume);
    req.price =
        (order_type == utilities::OrderType::MARKET) ? 0.0 : utilities::round_to(price, 0.01);
    req.reference = "Strategy_" + strategy_name;
    req.trading_class = contract->trading_class;
    std::string orderid = send_order(strategy_name, req);
    return orderid.empty() ? std::vector<std::string>{} : std::vector<std::string>{orderid};
}

auto OptionStrategyEngine::send_combo_order(const std::string& strategy_name,
                                            utilities::ComboType combo_type,
                                            const std::string& combo_sig,
                                            utilities::Direction direction, double price,
                                            double volume, const std::vector<utilities::Leg>& legs,
                                            utilities::OrderType order_type)
    -> std::vector<std::string> {
    if (!api_.send_combo_order || legs.empty()) {
        return {};
    }
    std::string orderid = api_.send_combo_order(strategy_name, combo_type, combo_sig, direction,
                                                price, volume, legs, order_type);
    if (!orderid.empty()) {
        strategy_active_orders_[strategy_name].insert(orderid);
        orderid_strategy_name_[orderid] = strategy_name;
        all_active_order_ids_.insert(orderid);
    }
    return orderid.empty() ? std::vector<std::string>{} : std::vector<std::string>{orderid};
}

void OptionStrategyEngine::init_strategy(const std::string& strategy_name) {
    auto* s = get_strategy(strategy_name);
    if (s == nullptr) {
        throw std::runtime_error("Strategy not found: " + strategy_name);
    }
    s->on_init();
    if (api_.put_strategy_event) {
        utilities::StrategyUpdateData u;
        u.strategy_name = strategy_name;
        u.class_name = extract_class_name(strategy_name);
        u.portfolio = extract_portfolio_name(strategy_name);
        u.json_payload = "{}";
        api_.put_strategy_event(u);
    }
}

void OptionStrategyEngine::start_strategy(const std::string& strategy_name) {
    auto* s = get_strategy(strategy_name);
    if (s == nullptr) {
        throw std::runtime_error("Strategy not found: " + strategy_name);
    }
    s->on_start();
    if (api_.put_strategy_event) {
        utilities::StrategyUpdateData u;
        u.strategy_name = strategy_name;
        u.class_name = extract_class_name(strategy_name);
        u.portfolio = extract_portfolio_name(strategy_name);
        u.json_payload = "{}";
        api_.put_strategy_event(u);
    }
}

void OptionStrategyEngine::stop_strategy(const std::string& strategy_name) {
    auto* s = get_strategy(strategy_name);
    if (s == nullptr) {
        throw std::runtime_error("Strategy not found: " + strategy_name);
    }
    s->on_stop();
    if (api_.put_strategy_event) {
        utilities::StrategyUpdateData u;
        u.strategy_name = strategy_name;
        u.class_name = extract_class_name(strategy_name);
        u.portfolio = extract_portfolio_name(strategy_name);
        u.json_payload = "{}";
        api_.put_strategy_event(u);
    }
}

auto OptionStrategyEngine::remove_strategy(const std::string& strategy_name) -> bool {
    auto it = strategies_.find(strategy_name);
    if (it == strategies_.end()) {
        return false;
    }
    if (it->second) {
        it->second->on_stop();
    }
    auto oit = strategy_active_orders_.find(strategy_name);
    if (oit != strategy_active_orders_.end()) {
        for (const std::string& oid : oit->second) {
            orderid_strategy_name_.erase(oid);
            all_active_order_ids_.erase(oid);
        }
        strategy_active_orders_.erase(oit);
    }
    strategies_.erase(it);
    if (api_.remove_strategy_holding) {
        api_.remove_strategy_holding(strategy_name);
    }
    if (api_.put_strategy_event) {
        utilities::StrategyUpdateData u;
        u.strategy_name = strategy_name;
        u.class_name = extract_class_name(strategy_name);
        u.portfolio = extract_portfolio_name(strategy_name);
        u.json_payload = "{}";
        api_.put_strategy_event(u);
    }
    return true;
}

void OptionStrategyEngine::add_strategy(const std::string& class_name,
                                        const std::string& portfolio_name,
                                        const std::unordered_map<std::string, double>& setting) {
    std::string strategy_name = class_name + "_" + portfolio_name;
    void* raw = strategy_cpp::StrategyRegistry::create(class_name, this, strategy_name,
                                                       portfolio_name, setting);
    if (raw == nullptr) {
        std::vector<std::string> available =
            strategy_cpp::StrategyRegistry::get_all_strategy_class_names();
        std::ostringstream os;
        os << "Unknown strategy '" << class_name << "'. Available: ";
        for (size_t i = 0; i < available.size(); ++i) {
            if (i != 0U) {
                os << ", ";
            }
            os << available[i];
        }
        throw std::runtime_error(os.str());
    }
    auto ptr = std::unique_ptr<strategy_cpp::OptionStrategyTemplate>(
        static_cast<strategy_cpp::OptionStrategyTemplate*>(raw));
    if (api_.get_or_create_holding) {
        api_.get_or_create_holding(strategy_name);
    }
    if (api_.get_holding) {
        ptr->set_holding(api_.get_holding(strategy_name));
    }
    strategy_active_orders_[strategy_name]; // ensure key exists
    strategies_[strategy_name] = std::move(ptr);
    if (api_.put_strategy_event) {
        utilities::StrategyUpdateData u;
        u.strategy_name = strategy_name;
        u.class_name = class_name;
        u.portfolio = portfolio_name;
        u.json_payload = "{}";
        api_.put_strategy_event(u);
    }
}

void OptionStrategyEngine::on_timer() {
    for (auto& [_, s] : strategies_) {
        if (s) {
            s->on_timer();
        }
    }
}

auto OptionStrategyEngine::get_order(const std::string& orderid) -> utilities::OrderData* {
    auto it = orders_.find(orderid);
    return (it != orders_.end()) ? &it->second : nullptr;
}

auto OptionStrategyEngine::get_trade(const std::string& tradeid) -> utilities::TradeData* {
    auto it = trades_.find(tradeid);
    return (it != trades_.end()) ? &it->second : nullptr;
}

auto OptionStrategyEngine::get_strategy_name_for_order(const std::string& orderid) const
    -> std::string {
    auto it = orderid_strategy_name_.find(orderid);
    return (it != orderid_strategy_name_.end()) ? it->second : std::string{};
}

auto OptionStrategyEngine::get_all_orders() const -> std::vector<utilities::OrderData> {
    std::vector<utilities::OrderData> out;
    for (const auto& [_, o] : orders_) {
        out.push_back(o);
    }
    return out;
}

auto OptionStrategyEngine::get_all_trades() const -> std::vector<utilities::TradeData> {
    std::vector<utilities::TradeData> out;
    for (const auto& [_, t] : trades_) {
        out.push_back(t);
    }
    return out;
}

auto OptionStrategyEngine::get_all_active_orders() const -> std::vector<utilities::OrderData> {
    std::vector<utilities::OrderData> out;
    for (const std::string& oid : all_active_order_ids_) {
        auto it = orders_.find(oid);
        if (it != orders_.end() && it->second.is_active()) {
            out.push_back(it->second);
        }
    }
    return out;
}

auto OptionStrategyEngine::get_strategy_active_orders()
    -> const std::unordered_map<std::string, std::set<std::string>>& {
    hedge_active_orders_cache_ = strategy_active_orders_;
    return hedge_active_orders_cache_;
}

auto OptionStrategyEngine::get_strategy_names() const -> std::vector<std::string> {
    std::vector<std::string> out;
    for (const auto& kv : strategies_) {
        out.push_back(kv.first);
    }
    return out;
}

void OptionStrategyEngine::close() {
    for (auto& [_, s] : strategies_) {
        if (s) {
            s->on_stop();
        }
    }
    strategies_.clear();
    strategy_active_orders_.clear();
    orderid_strategy_name_.clear();
    all_active_order_ids_.clear();
    orders_.clear();
    trades_.clear();
}

auto OptionStrategyEngine::combo_builder_engine() const -> engines::ComboBuilderEngine* {
    return api_.get_combo_builder_engine ? api_.get_combo_builder_engine() : nullptr;
}

auto OptionStrategyEngine::hedge_engine() const -> engines::HedgeEngine* {
    return api_.get_hedge_engine ? api_.get_hedge_engine() : nullptr;
}

void OptionStrategyEngine::remove_order_tracking(const std::string& orderid) {
    auto it = orderid_strategy_name_.find(orderid);
    if (it != orderid_strategy_name_.end()) {
        strategy_active_orders_[it->second].erase(orderid);
        orderid_strategy_name_.erase(it);
    }
    all_active_order_ids_.erase(orderid);
}

} // namespace core
