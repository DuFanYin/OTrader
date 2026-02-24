/**
 * C++ implementation of engines/engine_main.py
 */

#include "engine_main.hpp"
#include "../../core/engine_option_strategy.hpp"
#include "../../utilities/object.hpp"
#include "../../utilities/utility.hpp"
#include "engine_event.hpp"
#include <chrono>
#include <iomanip>
#include <sstream>

namespace engines {

MainEngine::MainEngine(EventEngine* event_engine) {
    if (!event_engine) {
        event_engine_ = std::make_unique<EventEngine>(1);
        event_engine_ptr_ = event_engine_.get();
    } else {
        event_engine_ptr_ = event_engine;
    }
    event_engine_ptr_->start();

    log_engine_ = std::make_unique<LogEngine>(this);
    position_engine_ = std::make_unique<PositionEngine>();
    db_engine_ = std::make_unique<DatabaseEngine>(this);
    market_data_engine_ = std::make_unique<MarketDataEngine>(this);
    ib_gateway_ = std::make_unique<IbGateway>(this);
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
    api.remove_strategy_holding = [this](const std::string& name) {
        if (position_engine_) position_engine_->remove_strategy_holding(name);
    };
    api.get_combo_builder_engine = [this]() { return combo_builder_engine(); };
    api.get_hedge_engine = [this]() { return hedge_engine(); };
    api.put_strategy_event = [this](const utilities::StrategyUpdateData& u) { on_strategy_event(u); };
    option_strategy_engine_ = std::make_unique<core::OptionStrategyEngine>(std::move(api));
    event_engine_ptr_->set_main_engine(this);

    // DatabaseEngine 从 PostgreSQL 加载合约并 put_event(Contract)，由 EventEngine 派发到 MarketDataEngine::process_contract，建立 portfolio
    db_engine_->load_contracts();

    write_log("Main engine initialization successful", INFO);
}

MainEngine::~MainEngine() {
    close();
}

void MainEngine::start_market_data_update() {
    market_data_engine_->start_market_data_update();
}

void MainEngine::stop_market_data_update() {
    market_data_engine_->stop_market_data_update();
}

void MainEngine::subscribe_chains(const std::string& strategy_name, const std::vector<std::string>& chain_symbols) {
    market_data_engine_->subscribe_chains(strategy_name, chain_symbols);
}

void MainEngine::unsubscribe_chains(const std::string& strategy_name) {
    market_data_engine_->unsubscribe_chains(strategy_name);
}

utilities::PortfolioData* MainEngine::get_portfolio(const std::string& portfolio_name) {
    return market_data_engine_->get_portfolio(portfolio_name);
}

std::vector<std::string> MainEngine::get_all_portfolio_names() const {
    return market_data_engine_->get_all_portfolio_names();
}

const utilities::ContractData* MainEngine::get_contract(const std::string& symbol) const {
    return market_data_engine_->get_contract(symbol);
}

std::vector<utilities::ContractData> MainEngine::get_all_contracts() const {
    return market_data_engine_->get_all_contracts();
}

void MainEngine::save_trade_data(const std::string& strategy_name, const utilities::TradeData& trade) {
    db_engine_->save_trade_data(strategy_name, trade);
}

void MainEngine::save_order_data(const std::string& strategy_name, const utilities::OrderData& order) {
    db_engine_->save_order_data(strategy_name, order);
}

void MainEngine::connect() {
    ib_gateway_->connect();
}

void MainEngine::disconnect() {
    ib_gateway_->disconnect();
}

void MainEngine::cancel_order(const utilities::CancelRequest& req) {
    if (option_strategy_engine_) option_strategy_engine_->remove_order_tracking(req.orderid);
    ib_gateway_->cancel_order(req);
}

std::string MainEngine::send_order(const utilities::OrderRequest& req) {
    return ib_gateway_->send_order(req);
}

void MainEngine::query_account() {
    ib_gateway_->query_account();
}

void MainEngine::query_position() {
    ib_gateway_->query_position();
}

utilities::OrderData* MainEngine::get_order(const std::string& orderid) {
    return option_strategy_engine_->get_order(orderid);
}

utilities::TradeData* MainEngine::get_trade(const std::string& tradeid) {
    return option_strategy_engine_->get_trade(tradeid);
}

void MainEngine::on_strategy_event(const utilities::StrategyUpdateData& update) {
    {
        std::lock_guard<std::mutex> lock(strategy_updates_mutex_);
        strategy_updates_.push_back(update);
        if (strategy_updates_.size() > 1000) {
            strategy_updates_.pop_front();
        }
    }
    strategy_updates_cv_.notify_one();
}

bool MainEngine::pop_strategy_update(utilities::StrategyUpdateData& out, int timeout_ms) {
    std::unique_lock<std::mutex> lock(strategy_updates_mutex_);
    if (!strategy_updates_cv_.wait_for(
            lock,
            std::chrono::milliseconds(timeout_ms),
            [this] { return !strategy_updates_.empty(); })) {
        return false;
    }
    out = std::move(strategy_updates_.front());
    strategy_updates_.pop_front();
    return true;
}

void MainEngine::put_event(const utilities::Event& e) {
    if (event_engine_ptr_) event_engine_ptr_->put_event(e);
}

void MainEngine::write_log(const std::string& msg, int level, const std::string& gateway) {
    utilities::LogData log;
    log.msg = msg;
    log.level = level;
    log.gateway_name = gateway.empty() ? "Main" : gateway;
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::ostringstream os;
    os << std::put_time(std::localtime(&t), "%m-%d %H:%M:%S");
    log.time = os.str();
    append_log(log);
}

void MainEngine::put_log_intent(const utilities::LogData& log) {
    if (log_engine_) log_engine_->process_log_intent(log);
}

void MainEngine::close() {
    option_strategy_engine_->close();
    db_engine_->close();
    ib_gateway_->disconnect();
    if (event_engine_ptr_) event_engine_ptr_->stop();
}

HedgeEngine* MainEngine::hedge_engine() {
    if (!hedge_engine_)
        hedge_engine_ = std::make_unique<HedgeEngine>();
    return hedge_engine_.get();
}

ComboBuilderEngine* MainEngine::combo_builder_engine() {
    if (!combo_builder_engine_)
        combo_builder_engine_ = std::make_unique<ComboBuilderEngine>();
    return combo_builder_engine_.get();
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

void MainEngine::set_log_level(int level) {
    if (log_engine_) log_engine_->set_level(level);
}

int MainEngine::log_level() const {
    return log_engine_ ? log_engine_->level() : DISABLED;
}

}  // namespace engines
