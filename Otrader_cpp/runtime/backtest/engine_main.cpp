#include "engine_main.hpp"
#include "engine_data_historical.hpp"
#include "../../core/engine_option_strategy.hpp"
#include "../../core/engine_hedge.hpp"
#include "../../strategy/template.hpp"
#include "../../utilities/utility.hpp"
#include <stdexcept>

namespace backtest {

namespace {
struct NoOpEventEngine : utilities::IEventEngine {};
NoOpEventEngine g_noop_event;
}

MainEngine::MainEngine(utilities::IEventEngine* event_engine) {
    if (!event_engine) event_engine = &g_noop_event;
    event_engine_ = event_engine;
    event_engine_->start();
    position_engine_ = std::make_unique<engines::PositionEngine>();
    log_engine_ = std::make_unique<engines::LogEngine>(this);
    log_engine_->set_level(engines::DISABLED);  // no log by default; call set_log_level(engines::INFO) etc. to enable
    core::RuntimeAPI api;
    api.send_order = [this](const std::string&, const utilities::OrderRequest& req) { return append_order(req); };
    api.send_combo_order = [this](const std::string&, utilities::ComboType combo_type,
                                  const std::string& combo_sig, utilities::Direction direction,
                                  double price, double volume, const std::vector<utilities::Leg>& legs,
                                  utilities::OrderType order_type) {
        utilities::OrderRequest req;
        req.symbol = "combo_" + combo_sig;
        req.exchange = utilities::Exchange::SMART;
        req.direction = direction;
        req.type = order_type;
        req.volume = volume;
        req.price = (order_type == utilities::OrderType::MARKET) ? 0.0 : utilities::round_to(price, 0.01);
        req.is_combo = true;
        req.combo_type = combo_type;
        req.legs = legs;
        if (!legs.empty() && legs.front().trading_class) req.trading_class = *legs.front().trading_class;
        return append_order(req);
    };
    api.write_log = [this](const utilities::LogData& log) { put_log_intent(log); };
    api.get_portfolio = [this](const std::string& name) { return get_portfolio(name); };
    api.get_contract = [this](const std::string& symbol) { return get_contract(symbol); };
    api.get_holding = [this](const std::string& name) { return get_holding(name); };
    api.get_or_create_holding = [this](const std::string& name) { get_or_create_holding(name); };
    api.get_combo_builder_engine = [this]() { return combo_builder_engine(); };
    api.get_hedge_engine = [this]() { return hedge_engine(); };
    option_strategy_engine_ = std::make_unique<core::OptionStrategyEngine>(std::move(api));
    put_log_intent("Main engine initialization successful", INFO);
}

MainEngine::~MainEngine() = default;

void MainEngine::register_portfolio(utilities::PortfolioData* portfolio) {
    if (portfolio) portfolios_[portfolio->name] = portfolio;
}

utilities::PortfolioData* MainEngine::get_portfolio(const std::string& portfolio_name) const {
    auto it = portfolios_.find(portfolio_name);
    return (it != portfolios_.end()) ? it->second : nullptr;
}

void MainEngine::register_contract(utilities::ContractData contract) {
    contracts_[contract.symbol] = std::move(contract);
}

const utilities::ContractData* MainEngine::get_contract(const std::string& symbol) const {
    auto it = contracts_.find(symbol);
    return (it != contracts_.end()) ? &it->second : nullptr;
}

BacktestDataEngine* MainEngine::load_backtest_data(const std::string& parquet_path,
                                                   const std::string& underlying_symbol) {
    if (!data_engine_)
        data_engine_ = std::make_unique<BacktestDataEngine>(this);
    data_engine_->load_parquet(parquet_path, "ts_recv", underlying_symbol);
    put_log_intent("Backtest data loaded from: " + parquet_path, INFO);
    return data_engine_.get();
}

void MainEngine::put_event(const utilities::Event& e) {
    if (event_engine_) event_engine_->put_event(e);
}

std::string MainEngine::send_order(const utilities::OrderRequest& req) {
    if (!order_executor_)
        throw std::runtime_error("No order executor set. Use BacktestEngine for backtest execution.");
    return order_executor_(req);
}

void MainEngine::add_order(std::string orderid, utilities::OrderData order) {
    orders_[std::move(orderid)] = std::move(order);
}

void MainEngine::cancel_order(const utilities::CancelRequest& req) {
    if (option_strategy_engine_)
        option_strategy_engine_->remove_order_tracking(req.orderid);
    auto it = orders_.find(req.orderid);
    if (it != orders_.end()) {
        it->second.status = utilities::Status::CANCELLED;
        put_event(utilities::Event(utilities::EventType::Order, it->second));
    }
}

utilities::OrderData* MainEngine::get_order(const std::string& orderid) const {
    if (!option_strategy_engine_) return nullptr;
    return option_strategy_engine_->get_order(orderid);
}

utilities::TradeData* MainEngine::get_trade(const std::string& tradeid) const {
    if (!option_strategy_engine_) return nullptr;
    return option_strategy_engine_->get_trade(tradeid);
}

std::vector<utilities::OrderData> MainEngine::get_all_orders() const {
    return option_strategy_engine_ ? option_strategy_engine_->get_all_orders() : std::vector<utilities::OrderData>{};
}

std::vector<utilities::TradeData> MainEngine::get_all_trades() const {
    return option_strategy_engine_ ? option_strategy_engine_->get_all_trades() : std::vector<utilities::TradeData>{};
}

std::vector<utilities::OrderData> MainEngine::get_all_active_orders() const {
    return option_strategy_engine_ ? option_strategy_engine_->get_all_active_orders() : std::vector<utilities::OrderData>{};
}

void MainEngine::put_log_intent(const std::string& msg, int level) const {
    utilities::LogData intent;
    intent.msg = msg;
    intent.level = level;
    intent.gateway_name = "Main";
    put_log_intent(intent);
}

void MainEngine::put_log_intent(const utilities::LogData& intent) const {
    if (!log_engine_) return;
    log_engine_->process_log_intent(intent);
}

void MainEngine::set_log_level(int level) {
    if (log_engine_) log_engine_->set_level(level);
}

int MainEngine::log_level() const {
    return log_engine_ ? log_engine_->level() : engines::DISABLED;
}

void MainEngine::close() {
    if (option_strategy_engine_)
        option_strategy_engine_->close();
    if (event_engine_)
        event_engine_->stop();
}

engines::ComboBuilderEngine* MainEngine::combo_builder_engine() {
    if (!combo_builder_engine_)
        combo_builder_engine_ = std::make_unique<engines::ComboBuilderEngine>();
    return combo_builder_engine_.get();
}

engines::HedgeEngine* MainEngine::hedge_engine() {
    if (!hedge_engine_)
        hedge_engine_ = std::make_unique<engines::HedgeEngine>();
    return hedge_engine_.get();
}

utilities::StrategyHolding* MainEngine::get_holding(const std::string& strategy_name) {
    return position_engine_ ? &position_engine_->get_holding(strategy_name) : nullptr;
}

const utilities::StrategyHolding* MainEngine::get_holding(const std::string& strategy_name) const {
    return const_cast<MainEngine*>(this)->get_holding(strategy_name);
}

void MainEngine::get_or_create_holding(const std::string& strategy_name) {
    if (position_engine_) position_engine_->get_create_strategy_holding(strategy_name);
}

std::string MainEngine::append_order(const utilities::OrderRequest& req) {
    return send_order(req);
}

void MainEngine::append_cancel(const utilities::CancelRequest& req) {
    cancel_order(req);
}

void MainEngine::append_log(const utilities::LogData& log) {
    put_log_intent(log);
}

}  // namespace backtest
