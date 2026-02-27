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
    std::string strategy_name = api_.execution.get_strategy_name_for_order
                                    ? api_.execution.get_strategy_name_for_order(order.orderid)
                                    : std::string{};
    if (!strategy_name.empty()) {
        auto* s = get_strategy(strategy_name);
        if (s != nullptr) {
            s->on_order(order);
        }
    }
}

void OptionStrategyEngine::process_trade(const utilities::TradeData& trade) {
    std::string strategy_name = api_.execution.get_strategy_name_for_order
                                    ? api_.execution.get_strategy_name_for_order(trade.orderid)
                                    : std::string{};
    if (!strategy_name.empty()) {
        auto* s = get_strategy(strategy_name);
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
    if (!api_.portfolio.get_holding) {
        return nullptr;
    }
    return api_.portfolio.get_holding(strategy_name);
}

auto OptionStrategyEngine::get_strategy_holding() const -> utilities::StrategyHolding* {
    if (strategies_.size() != 1) {
        return nullptr;
    }
    return get_strategy_holding(strategies_.begin()->first);
}

auto OptionStrategyEngine::get_portfolio(const std::string& portfolio_name) const
    -> utilities::PortfolioData* {
    return api_.portfolio.get_portfolio ? api_.portfolio.get_portfolio(portfolio_name) : nullptr;
}

auto OptionStrategyEngine::get_holding(const std::string& strategy_name) const
    -> utilities::StrategyHolding* {
    return api_.portfolio.get_holding ? api_.portfolio.get_holding(strategy_name) : nullptr;
}

auto OptionStrategyEngine::get_contract(const std::string& symbol) const
    -> const utilities::ContractData* {
    return api_.portfolio.get_contract ? api_.portfolio.get_contract(symbol) : nullptr;
}

void OptionStrategyEngine::write_log(const std::string& msg, int level) const {
    if (api_.system.write_log) {
        utilities::LogData log;
        log.msg = msg;
        log.level = level;
        log.gateway_name = "Strategy";
        api_.system.write_log(log);
    }
}

void OptionStrategyEngine::write_log(const utilities::LogData& log) const {
    if (api_.system.write_log) {
        api_.system.write_log(log);
    }
}

auto OptionStrategyEngine::send_order(const std::string& strategy_name,
                                      const utilities::OrderRequest& req) const -> std::string {
    if (!api_.execution.send_order) {
        return {};
    }
    return api_.execution.send_order ? api_.execution.send_order(strategy_name, req)
                                     : std::string{};
}

auto OptionStrategyEngine::send_order(const std::string& strategy_name, const std::string& symbol,
                                      utilities::Direction direction, double price, double volume,
                                      utilities::OrderType order_type) -> std::vector<std::string> {
    if (!api_.execution.send_order) {
        return {};
    }
    utilities::OrderRequest req;
    if (!assemble_order_request(strategy_name, symbol, direction, price, volume, order_type,
                                nullptr, std::nullopt, nullptr, req)) {
        return {};
    }
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
    if (legs.empty()) {
        return {};
    }
    utilities::OrderRequest req;
    if (!assemble_order_request(strategy_name, /*symbol unused*/ "", direction, price, volume,
                                order_type, &legs, combo_type, &combo_sig, req)) {
        return {};
    }
    std::string orderid = send_order(strategy_name, req);
    return orderid.empty() ? std::vector<std::string>{} : std::vector<std::string>{orderid};
}

auto OptionStrategyEngine::assemble_order_request(
    const std::string& strategy_name, const std::string& symbol, utilities::Direction direction,
    double price, double volume, utilities::OrderType order_type,
    const std::vector<utilities::Leg>* legs, std::optional<utilities::ComboType> combo_type,
    const std::string* combo_sig, utilities::OrderRequest& out_req) const -> bool {
    // 组合单：legs 非空，使用 combo_sig/combo_type 组装。
    if (legs != nullptr && !legs->empty() && combo_type.has_value() && combo_sig != nullptr) {
        out_req.symbol = "combo_" + *combo_sig;
        out_req.exchange = utilities::Exchange::SMART;
        out_req.direction = direction;
        out_req.type = order_type;
        out_req.volume = volume;
        out_req.price =
            (order_type == utilities::OrderType::MARKET) ? 0.0 : utilities::round_to(price, 0.01);
        out_req.is_combo = true;
        out_req.combo_type = *combo_type;
        out_req.legs = *legs;
        if (!legs->empty() && legs->front().trading_class) {
            out_req.trading_class = *legs->front().trading_class;
        }
        out_req.reference = "Strategy_" + strategy_name;
        return true;
    }

    // 单腿：legs 为空，按 symbol 查合约信息。
    const utilities::ContractData* contract = get_contract(symbol);
    if (contract == nullptr) {
        return false;
    }
    out_req.symbol = contract->symbol;
    out_req.exchange = contract->exchange;
    out_req.direction = direction;
    out_req.type = order_type;
    out_req.volume = utilities::round_to(volume, contract->min_volume);
    out_req.price =
        (order_type == utilities::OrderType::MARKET) ? 0.0 : utilities::round_to(price, 0.01);
    out_req.reference = "Strategy_" + strategy_name;
    out_req.trading_class = contract->trading_class;
    out_req.is_combo = false;
    out_req.legs.reset();
    out_req.combo_type.reset();
    return true;
}

void OptionStrategyEngine::init_strategy(const std::string& strategy_name) {
    auto* s = get_strategy(strategy_name);
    if (s == nullptr) {
        throw std::runtime_error("Strategy not found: " + strategy_name);
    }
    s->on_init();
    if (api_.system.put_strategy_event) {
        utilities::StrategyUpdateData u;
        u.strategy_name = strategy_name;
        u.class_name = extract_class_name(strategy_name);
        u.portfolio = extract_portfolio_name(strategy_name);
        u.json_payload = "{}";
        api_.system.put_strategy_event(u);
    }
}

void OptionStrategyEngine::start_strategy(const std::string& strategy_name) {
    auto* s = get_strategy(strategy_name);
    if (s == nullptr) {
        throw std::runtime_error("Strategy not found: " + strategy_name);
    }
    s->on_start();
    if (api_.system.put_strategy_event) {
        utilities::StrategyUpdateData u;
        u.strategy_name = strategy_name;
        u.class_name = extract_class_name(strategy_name);
        u.portfolio = extract_portfolio_name(strategy_name);
        u.json_payload = "{}";
        api_.system.put_strategy_event(u);
    }
}

void OptionStrategyEngine::stop_strategy(const std::string& strategy_name) {
    auto* s = get_strategy(strategy_name);
    if (s == nullptr) {
        throw std::runtime_error("Strategy not found: " + strategy_name);
    }
    s->on_stop();
    if (api_.system.put_strategy_event) {
        utilities::StrategyUpdateData u;
        u.strategy_name = strategy_name;
        u.class_name = extract_class_name(strategy_name);
        u.portfolio = extract_portfolio_name(strategy_name);
        u.json_payload = "{}";
        api_.system.put_strategy_event(u);
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
    if (api_.execution.remove_strategy_tracking) {
        api_.execution.remove_strategy_tracking(strategy_name);
    }
    strategies_.erase(it);
    if (api_.portfolio.remove_strategy_holding) {
        api_.portfolio.remove_strategy_holding(strategy_name);
    }
    if (api_.system.put_strategy_event) {
        utilities::StrategyUpdateData u;
        u.strategy_name = strategy_name;
        u.class_name = extract_class_name(strategy_name);
        u.portfolio = extract_portfolio_name(strategy_name);
        u.json_payload = "{}";
        api_.system.put_strategy_event(u);
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
    if (api_.portfolio.get_or_create_holding) {
        api_.portfolio.get_or_create_holding(strategy_name);
    }
    if (api_.portfolio.get_holding) {
        ptr->set_holding(api_.portfolio.get_holding(strategy_name));
    }
    if (api_.execution.ensure_strategy_key) {
        api_.execution.ensure_strategy_key(strategy_name);
    }
    strategies_[strategy_name] = std::move(ptr);
    if (api_.system.put_strategy_event) {
        utilities::StrategyUpdateData u;
        u.strategy_name = strategy_name;
        u.class_name = class_name;
        u.portfolio = portfolio_name;
        u.json_payload = "{}";
        api_.system.put_strategy_event(u);
    }
}

void OptionStrategyEngine::on_timer() {
    for (auto& [_, s] : strategies_) {
        if (s) {
            s->on_timer();
        }
    }
}

auto OptionStrategyEngine::get_order(const std::string& orderid) const -> utilities::OrderData* {
    return api_.execution.get_order ? api_.execution.get_order(orderid) : nullptr;
}

auto OptionStrategyEngine::get_trade(const std::string& tradeid) const -> utilities::TradeData* {
    return api_.execution.get_trade ? api_.execution.get_trade(tradeid) : nullptr;
}

auto OptionStrategyEngine::get_strategy_name_for_order(const std::string& orderid) const
    -> std::string {
    return api_.execution.get_strategy_name_for_order
               ? api_.execution.get_strategy_name_for_order(orderid)
               : std::string{};
}

auto OptionStrategyEngine::get_all_orders() const -> std::vector<utilities::OrderData> {
    return api_.execution.get_all_orders ? api_.execution.get_all_orders()
                                         : std::vector<utilities::OrderData>{};
}

auto OptionStrategyEngine::get_all_trades() const -> std::vector<utilities::TradeData> {
    return api_.execution.get_all_trades ? api_.execution.get_all_trades()
                                         : std::vector<utilities::TradeData>{};
}

auto OptionStrategyEngine::get_all_active_orders() const -> std::vector<utilities::OrderData> {
    return api_.execution.get_all_active_orders ? api_.execution.get_all_active_orders()
                                                : std::vector<utilities::OrderData>{};
}

auto OptionStrategyEngine::get_strategy_active_orders() const
    -> const std::unordered_map<std::string, std::set<std::string>>& {
    static const std::unordered_map<std::string, std::set<std::string>> empty;
    return api_.execution.get_strategy_active_orders ? api_.execution.get_strategy_active_orders()
                                                     : empty;
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
}

auto OptionStrategyEngine::combo_builder_engine() const -> engines::ComboBuilderEngine* {
    return api_.system.get_combo_builder_engine ? api_.system.get_combo_builder_engine() : nullptr;
}

auto OptionStrategyEngine::hedge_engine() const -> engines::HedgeEngine* {
    return api_.system.get_hedge_engine ? api_.system.get_hedge_engine() : nullptr;
}

void OptionStrategyEngine::remove_order_tracking(const std::string& orderid) const {
    if (api_.execution.remove_order_tracking) {
        api_.execution.remove_order_tracking(orderid);
    }
}

} // namespace core
