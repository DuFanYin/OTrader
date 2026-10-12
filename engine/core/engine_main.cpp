#include "core/engine_main.hpp"

#include "core/runtime_api.hpp"
#include "strategy/strategy_registry.hpp"
#include "strategy/template.hpp"
#include "model/symbol.hpp"
#include "model/utility.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <format>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <variant>

namespace core {

namespace {

/** An expired option settles at the underlying's last quote when it is this close to the close:
 * a quote from much earlier or later (the next morning's) is not the closing price. */
constexpr auto kSettleWindow = std::chrono::minutes(15);

/** What the operator last asked of the strategy, as its template has it. */
auto state_of(const strategy::OptionStrategyTemplate& s) -> adapters::StrategyState {
    return s.started()  ? adapters::StrategyState::Running
           : s.inited() ? adapters::StrategyState::Inited
                        : adapters::StrategyState::Added;
}

auto join(const std::vector<std::string>& items) -> std::string {
    std::string out;
    for (const std::string& item : items) {
        out += (out.empty() ? "" : ", ") + item;
    }
    return out;
}

} // namespace

Engine::Engine()
    : log_engine_(std::make_unique<core::LogEngine>()),
      portfolio_structure_(std::make_unique<model::PortfolioStructure>()) {}
Engine::~Engine() = default;

void Engine::init_core(utilities::LogLevel log_level) {
    log_engine_->set_level(log_level);
    position_engine_ = std::make_unique<core::PositionEngine>(log_engine_.get());
    execution_engine_ = std::make_unique<core::ExecutionEngine>(log_engine_.get());
    risk_engine_ = std::make_unique<core::RiskEngine>(log_engine_.get(), *portfolio_structure_,
                                                      *position_engine_, *execution_engine_);
    restore_risk();

    // The venue's order updates and fills come in as events; orders go out through it
    // (send_order).
    gateway_->set_callbacks(
        [this](const model::OrderData& o) {
            post(model::EventType::Order, acquire_order(), o);
        },
        [this](const model::TradeData& t) {
            post(model::EventType::Trade, acquire_trade(), t);
        });

    // What the core engines ask of this one (RuntimeAPI): orders through the venue, the market
    // data, the control plane's stream, the storage. They reach each other directly.
    hedge_engine_ = std::make_unique<core::HedgeEngine>(
        log_engine_.get(), *portfolio_structure_, *position_engine_, *execution_engine_, *this);
    option_strategy_engine_ = std::make_unique<core::OptionStrategyEngine>(
        log_engine_.get(), *portfolio_structure_, *execution_engine_, *position_engine_,
        *hedge_engine_, *this);
}

// ---- Shared method implementations ----

auto Engine::get_portfolio(const std::string& name) -> model::PortfolioData* {
    return portfolio_structure_->get_portfolio(name);
}

auto Engine::get_contract(const std::string& symbol) const -> const model::ContractData* {
    return portfolio_structure_->get_contract(symbol);
}

auto Engine::get_order(const std::string& orderid) -> model::OrderData* {
    return execution_engine_ ? execution_engine_->get_order(orderid) : nullptr;
}

auto Engine::get_holding(const std::string& strategy_name) -> model::StrategyHolding* {
    return position_engine_ ? &position_engine_->get_holding(strategy_name) : nullptr;
}

auto Engine::get_holding(const std::string& strategy_name) const
    -> const model::StrategyHolding* {
    return const_cast<Engine*>(this)->get_holding(strategy_name);
}

void Engine::write_log(const std::string& msg, utilities::LogLevel level) const {
    utilities::LogData record;
    record.source = "Main";
    record.msg = msg;
    record.level = level;
    log_engine_->log(record);
}

void Engine::set_log_level(utilities::LogLevel level) { log_engine_->set_level(level); }

auto Engine::log_level() const -> utilities::LogLevel { return log_engine_->level(); }

// ---- Booking ----

void Engine::book_order(model::OrderData& order) {
    if (!execution_engine_->defer_terminal_update(order, now())) {
        apply_order(order);
    }
}

void Engine::apply_order(const model::OrderData& update) {
    const std::string strategy_name =
        execution_engine_->get_strategy_name_for_order(update.orderid);
    const model::OrderData* merged = execution_engine_->process_order_event(strategy_name, update);
    if (merged == nullptr) {
        return; // nothing new: a repeat, or an older update come late
    }
    model::OrderData known = *merged;
    if (!strategy_name.empty()) {
        save_order_data(strategy_name, known);
    }
    option_strategy_engine_->process_order_event(known);
}

void Engine::book_trade(const model::TradeData& trade) {
    if (execution_engine_->get_trade(trade.tradeid) != nullptr) {
        return; // booked already: IB repeats today's fills when the engine reconnects
    }
    execution_engine_->process_trade_event(trade);
    const std::string strategy_name =
        execution_engine_->get_strategy_name_for_order(trade.orderid);
    if (strategy_name.empty()) {
        // An order no strategy sent (placed by hand, say): the check against the broker reports
        // what it changed.
        write_log(std::format("Fill {} of order {} ({} {}) is no strategy's: not held",
                              trade.tradeid, trade.orderid, trade.volume, trade.symbol),
                  utilities::WARNING);
        return;
    }
    save_trade_data(strategy_name, trade);
    // The order as sent (stored by Engine::send_order before any venue update).
    position_engine_->process_trade_event(strategy_name, trade,
                                          execution_engine_->get_order(trade.orderid));
    on_positions_changed();
    option_strategy_engine_->process_trade_event(trade);
    if (const std::optional<model::OrderData> update =
            execution_engine_->take_ready_terminal_update(trade)) {
        apply_order(*update);
    }
}

// ---- The timer ----

void Engine::process_event(const model::Event& event) {
    switch (event.type) {
        using enum model::EventType;
    case Snapshot:
        if (const auto* slot = std::get_if<model::PortfolioSnapshot*>(&event.data);
            slot != nullptr && *slot != nullptr) {
            snapshot_time_ = (*slot)->datetime;
            if (model::PortfolioData* portfolio = get_portfolio((*slot)->portfolio_name)) {
                portfolio->apply_frame(**slot);
                option_strategy_engine_->process_snapshot_event((*slot)->portfolio_name);
            }
        }
        break;
    case Timer:
        on_timer(now());
        break;
    case Order:
        if (const auto* slot = std::get_if<model::OrderData*>(&event.data);
            slot != nullptr && *slot != nullptr) {
            book_order(**slot);
        }
        break;
    case Trade:
        if (const auto* slot = std::get_if<model::TradeData*>(&event.data);
            slot != nullptr && *slot != nullptr) {
            book_trade(**slot);
        }
        break;
    case Task:
        if (const auto* task = std::get_if<model::Task*>(&event.data);
            task != nullptr && *task != nullptr) {
            (**task)();
        }
        break;
    }
}

void Engine::on_timer(model::DateTime now) {
    for (const model::OrderData& update : execution_engine_->take_expired_terminal_updates(now)) {
        write_log(std::format("Order {}: terminal update ({} traded) applied after {} without all "
                              "its fills booked",
                              update.orderid, update.traded,
                              core::ExecutionEngine::kTerminalUpdateTimeout),
                  utilities::WARNING);
        apply_order(update);
    }
    settle_due(now);
    flatten_expiring(now);
    option_strategy_engine_->process_timer_event();
    for (const std::string& name : option_strategy_engine_->get_strategy_names_ref()) {
        const auto* s = option_strategy_engine_->get_strategy(name);
        if (const model::PortfolioData* portfolio =
                s != nullptr ? get_portfolio(s->portfolio_name()) : nullptr) {
            position_engine_->update_metrics(name, *portfolio);
        }
    }
    hedge_engine_->process_timer_event(option_strategy_engine_->get_strategy_names_ref());
}

// ---- Expiry ----

void Engine::settle_due(model::DateTime now) {
    std::unordered_map<std::string, std::vector<std::string>> waiting;
    for (const std::string& name : option_strategy_engine_->get_strategy_names()) {
        const strategy::OptionStrategyTemplate* strategy =
            option_strategy_engine_->get_strategy(name);
        if (strategy == nullptr) {
            continue;
        }
        std::map<std::chrono::year_month_day, std::vector<std::string>> by_expiry;
        for (const std::string& symbol : position_engine_->expired_options(name, now)) {
            if (const auto id = model::parse_option_symbol(symbol)) {
                by_expiry[id->expiry].push_back(symbol);
            }
        }
        if (by_expiry.empty()) {
            continue;
        }
        const model::PortfolioData* portfolio = get_portfolio(strategy->portfolio_name());
        const double price = (portfolio != nullptr && portfolio->underlying)
                                 ? portfolio->underlying->mid_price
                                 : 0.0;
        for (const auto& [expiry, symbols] : by_expiry) {
            const auto close = model::option_expiry_time(expiry);
            if (price > 0.0 && std::chrono::abs(portfolio->datetime() - close) <= kSettleWindow) {
                for (const auto& [order, fills] :
                     position_engine_->settle_expiry(name, expiry, price, *portfolio)) {
                    book_settlement(name, order, fills);
                }
            } else {
                auto& w = waiting[name];
                w.insert(w.end(), symbols.begin(), symbols.end());
            }
        }
    }
    for (const auto& [name, symbols] : waiting) {
        if (unsettled_options_[name] != symbols) {
            write_log(std::format("{} holds expired options without a closing price to settle at"
                                  " ({}): SettleExpired with the underlying's close",
                                  name, join(symbols)),
                      utilities::WARNING);
        }
    }
    unsettled_options_ = std::move(waiting);
}

auto Engine::settle_expired(const std::string& portfolio,
                                    std::chrono::year_month_day expiry, double price,
                                    model::DateTime now) -> int {
    const model::PortfolioData* p = get_portfolio(portfolio);
    if (p == nullptr) {
        throw std::invalid_argument("No portfolio " + portfolio);
    }
    if (!expiry.ok() || price <= 0.0) {
        throw std::invalid_argument("Give the expiry date and the underlying's closing price");
    }
    if (model::option_expiry_time(expiry) > now) {
        throw std::invalid_argument(std::format("{:%F} has not closed yet", expiry));
    }
    int count = 0;
    for (const std::string& name : option_strategy_engine_->get_strategy_names()) {
        const auto* strategy = option_strategy_engine_->get_strategy(name);
        if (strategy == nullptr || strategy->portfolio_name() != portfolio) {
            continue;
        }
        for (const auto& [order, fills] : position_engine_->settle_expiry(name, expiry, price, *p)) {
            book_settlement(name, order, fills);
            ++count;
        }
    }
    return count;
}

void Engine::book_settlement(const std::string& strategy_name,
                                     const model::OrderData& order,
                                     std::span<const model::TradeData> fills) {
    write_log(std::format("{}: settled {} {} at {} ({})", strategy_name, order.volume,
                          order.symbol, order.price,
                          fills.size() > 1 ? "its legs" : order.reference),
              utilities::INFO);
    book_filled_order(strategy_name, order, fills);
}

void Engine::book_filled_order(const std::string& strategy_name, const model::OrderData& order,
                               std::span<const model::TradeData> fills) {
    execution_engine_->register_active_order(strategy_name, order.orderid); // the strategy's
    model::OrderData booked = order;
    book_order(booked); // deferred until the fills below are booked
    for (const model::TradeData& t : fills) {
        book_trade(t);
    }
}

void Engine::flatten_expiring(model::DateTime now) {
    if (risk_engine_->setting("flatten_minutes") <= 0.0) {
        return;
    }
    for (const std::string& name : option_strategy_engine_->get_strategy_names()) {
        strategy::OptionStrategyTemplate* s = option_strategy_engine_->get_strategy(name);
        const model::StrategyHolding* holding = position_engine_->find_holding(name);
        model::PortfolioData* portfolio = s != nullptr ? get_portfolio(s->portfolio_name()) : nullptr;
        if (holding == nullptr || portfolio == nullptr) {
            continue;
        }
        // An order already working on it (the closing one sent a tick ago): left to fill.
        std::set<std::string> working;
        for (const std::string& orderid : execution_engine_->active_orders(name)) {
            if (const model::OrderData* o = execution_engine_->get_order(orderid)) {
                working.insert(o->symbol);
                for (const model::Leg& leg : o->legs) {
                    working.insert(leg.symbol);
                }
            }
        }
        const auto due = [&](const model::BasePosition& p) {
            return p.quantity != 0 && !working.contains(p.symbol) &&
                   risk_engine_->in_flatten_window(p.symbol, portfolio, now);
        };
        for (const auto& [key, position] : holding->optionPositions) {
            // An option, or the legs of a combo that are due: closed as that position (a combo's
            // other legs stay open).
            model::OptionPositionData closing = position;
            if (closing.legs.empty()) {
                if (!due(closing)) {
                    continue;
                }
            } else {
                std::erase_if(closing.legs,
                              [&](const model::OptionPositionData& leg) { return !due(leg); });
                if (closing.legs.empty()) {
                    continue;
                }
            }
            write_log(std::format("{}: closing {} {}: it settles in shares at today's close", name,
                                  closing.quantity, key),
                      utilities::WARNING);
            s->close_strategy_position(closing);
        }
    }
}

void Engine::close_ports() {
    if (market_data_) {
        market_data_->stop();
    }
    if (gateway_) {
        gateway_->close();
    }
    if (event_engine_) {
        event_engine_->close();
    }
    if (db_engine_) {
        db_engine_->close();
    }
}

void Engine::close() {
    close_ports();
    if (option_strategy_engine_) {
        option_strategy_engine_->close();
    }
    if (execution_engine_) {
        execution_engine_->close();
    }
}

auto Engine::get_all_orders() const -> std::vector<model::OrderData> {
    return execution_engine_->get_all_orders();
}

auto Engine::get_all_trades() const -> std::vector<model::TradeData> {
    return execution_engine_->get_all_trades();
}

auto Engine::get_all_active_orders() const -> std::vector<model::OrderData> {
    return execution_engine_->get_all_active_orders();
}

// ---- The broker, the strategies, the storage ----

void Engine::save_order_data(const std::string& strategy_name,
                                 const model::OrderData& order) {
    db_engine_->save_order_data(strategy_name, order);
}

void Engine::save_trade_data(const std::string& strategy_name,
                                 const model::TradeData& trade) {
    db_engine_->save_trade_data(strategy_name, trade);
}

auto Engine::get_all_portfolio_names() const -> std::vector<std::string> {
    return portfolio_structure_->get_all_portfolio_names();
}

auto Engine::get_all_contracts() const -> std::vector<model::ContractData> {
    return portfolio_structure_->get_all_contracts();
}

auto Engine::get_strategy_errors() -> std::vector<std::pair<std::string, std::string>> {
    return on_event_thread([&] { return option_strategy_engine()->get_strategy_errors(); });
}

auto Engine::connected() const -> bool {
    return gateway_ && gateway_->is_connected();
}

void Engine::connect() {
    gateway_->connect();
    if (!gateway_->has_broker()) {
        return;
    }
    // What happened while the engine was down, then the account's positions against the
    // holdings. The fills come in as events before the catch-up is done, so they are booked by
    // the time the comparison runs behind them on the event thread. The gateway fails a request
    // IB does not answer in time.
    try {
        gateway_->catch_up().get();
        reconcile_positions();
    } catch (const std::exception& e) {
        on_event_thread([&] { position_engine_->record_broker_check_failed(e.what()); });
    }
}

void Engine::disconnect() { gateway_->disconnect(); }

auto Engine::send_order(const model::OrderRequest& req) -> std::string {
    if (!risk_engine_->allow({}, {}, req, now())) {
        return {};
    }
    return gateway_->send_order(req);
}

auto Engine::send_order(const std::string& strategy_name, const model::OrderRequest& req)
    -> std::string {
    const strategy::OptionStrategyTemplate* strategy =
        option_strategy_engine_->get_strategy(strategy_name);
    if (!risk_engine_->allow(strategy_name, strategy != nullptr ? strategy->portfolio_name() : "",
                             req, now())) {
        return {};
    }
    // Stored as sent (contract, legs) before any venue update, so that every fill finds its order.
    std::string orderid = gateway_->send_order(req);
    if (!orderid.empty()) {
        const model::OrderData sent = req.create_order_data(orderid);
        execution_engine_->register_sent_order(strategy_name, sent);
        save_order_data(strategy_name, sent);
    }
    return orderid;
}

void Engine::cancel_order(const model::CancelRequest& req) {
    gateway_->cancel_order(req);
    execution_engine_->remove_order_tracking(req.orderid);
}

auto Engine::query_account() -> adapters::BrokerAccount { return gateway_->query_account().get(); }

auto Engine::sync_contracts(const std::string& portfolio, int max_dte)
    -> std::vector<model::ContractData> {
    // The gateway fails the sync if IB stops answering or the connection drops.
    std::vector<model::ContractData> contracts =
        gateway_->sync_contracts(portfolio, max_dte).get();
    db_engine_->save_contracts(contracts);
    write_log(std::format("Saved {} {} contracts: restart the engine to trade them",
                          contracts.size(), portfolio),
              utilities::INFO);
    return contracts;
}

void Engine::restore() {
    core::OptionStrategyEngine* se = option_strategy_engine();
    std::unordered_set<std::string> loaded;
    std::vector<std::string> to_init;
    for (const adapters::StrategyRecord& r : db_engine_->load_strategies()) {
        if (r.state == adapters::StrategyState::Removed) {
            continue;
        }
        try {
            se->restore_strategy(r.class_name, r.portfolio, r.setting, r.name);
            loaded.insert(r.name);
            if (r.state != adapters::StrategyState::Added) {
                to_init.push_back(r.name);
            }
        } catch (const std::exception& e) {
            write_log(std::format("Cannot restore {} ({} on {}): {}", r.name, r.class_name,
                                  r.portfolio, e.what()),
                      utilities::ERROR);
        }
    }
    if (loaded.empty()) {
        return;
    }
    rebook(loaded);
    for (const std::string& name : to_init) { // on_init sees its holding
        try {
            se->init_strategy(name);
        } catch (const std::exception& e) {
            write_log(std::format("Cannot init {}: {}", name, e.what()), utilities::ERROR);
        }
    }
    on_positions_changed();
    write_log(std::format("Restored {} strategies ({} inited); start them, or resume()",
                          loaded.size(), to_init.size()),
              utilities::INFO);
}

void Engine::rebook(const std::unordered_set<std::string>& strategies) {
    // Every order and fill saved, in the order saved: the holdings as they were, and each fill
    // known, so that one IB reports again (it repeats today's on a connect) is dropped.
    const std::vector<std::string> names(strategies.begin(), strategies.end());
    std::vector<model::OrderData> working;
    std::size_t orders = 0;
    for (adapters::SavedRecord& saved : db_engine_->load_orders(names)) {
        auto& order = std::get<model::OrderData>(saved.record);
        execution_engine_->restore_order(saved.strategy_name, order);
        ++orders;
        if (order.is_active()) {
            working.push_back(std::move(order));
        }
    }
    std::size_t trades = 0;
    for (const adapters::SavedRecord& saved : db_engine_->load_trades(names)) {
        const auto& trade = std::get<model::TradeData>(saved.record);
        execution_engine_->process_trade_event(trade);
        position_engine_->process_trade_event(saved.strategy_name, trade,
                                              execution_engine_->get_order(trade.orderid));
        ++trades;
    }
    gateway_->restore_orders(working); // they may fill: they stay theirs
    write_log(std::format("Rebooked {} orders ({} working) and {} fills", orders, working.size(),
                          trades),
              utilities::INFO);
}

auto Engine::add_strategy(const std::string& class_name, const std::string& portfolio,
                              const std::unordered_map<std::string, double>& setting,
                              const std::string& strategy_name) -> std::string {
    return on_event_thread([&] {
        core::OptionStrategyEngine* se = option_strategy_engine();
        const std::string name = se->add_strategy(class_name, portfolio, setting, strategy_name);
        db_engine_->save_strategy(adapters::StrategyRecord{.name = name,
                                                 .class_name = class_name,
                                                 .portfolio = portfolio,
                                                 .setting = se->get_strategy(name)->setting(),
                                                 .state = adapters::StrategyState::Added});
        return name;
    });
}

void Engine::init_strategy(const std::string& name) {
    on_event_thread([&] {
        option_strategy_engine()->init_strategy(name);
        save_strategy_state(name);
    });
}

void Engine::start_strategy(const std::string& name) {
    on_event_thread([&] {
        if (risk_engine_->halted()) {
            throw std::runtime_error("Trading is halted (" + risk_engine_->halt_reason() +
                                     "): clear the halt first");
        }
        if (const auto refusal = position_engine_->start_refusal(
                name, now(),
                gateway_->has_broker() && !reconcile_warn_only_)) {
            throw std::runtime_error(*refusal);
        }
        option_strategy_engine()->start_strategy(name);
        save_strategy_state(name);
    });
}

void Engine::stop_strategy(const std::string& name) {
    on_event_thread([&] {
        option_strategy_engine()->stop_strategy(name);
        save_strategy_state(name);
    });
}

void Engine::save_strategy_state(const std::string& name) {
    if (const strategy::OptionStrategyTemplate* s = option_strategy_engine()->get_strategy(name)) {
        db_engine_->set_strategy_state(name, state_of(*s));
    }
}

auto Engine::strategies() -> std::vector<adapters::StrategyRecord> {
    return on_event_thread([&] {
        core::OptionStrategyEngine* se = option_strategy_engine();
        std::vector<adapters::StrategyRecord> out;
        for (const std::string& name : se->get_strategy_names()) {
            if (const strategy::OptionStrategyTemplate* s = se->get_strategy(name)) {
                out.push_back(adapters::StrategyRecord{.name = name,
                                                    .class_name = se->get_strategy_class_name(name),
                                                    .portfolio = s->portfolio_name(),
                                                    .setting = s->setting(),
                                                    .state = state_of(*s)});
            }
        }
        return out;
    });
}

auto Engine::holdings() -> std::unordered_map<std::string, model::StrategyHolding> {
    return on_event_thread([&] {
        std::unordered_map<std::string, model::StrategyHolding> out;
        for (const std::string& name : option_strategy_engine()->get_strategy_names()) {
            if (const model::StrategyHolding* holding = position_engine_->find_holding(name)) {
                out.emplace(name, *holding);
            }
        }
        return out;
    });
}

auto Engine::strategy_classes() -> std::vector<std::string> {
    return strategy::StrategyRegistry::get_all_strategy_class_names();
}

auto Engine::strategy_class_defaults(const std::string& class_name) const
    -> std::unordered_map<std::string, double> {
    return option_strategy_engine()->get_default_setting(class_name);
}

void Engine::remove_strategy(const std::string& name) {
    on_event_thread([&] {
        if (!option_strategy_engine()->remove_strategy(name)) {
            throw std::invalid_argument("No strategy " + name);
        }
        db_engine_->set_strategy_state(name, adapters::StrategyState::Removed);
    });
}

void Engine::delete_strategy(const std::string& name) {
    on_event_thread([&] {
        if (option_strategy_engine()->get_strategy(name) != nullptr) {
            remove_strategy(name);
        }
        db_engine_->delete_strategy(name);
    });
}

auto Engine::removed_strategies() -> std::vector<std::string> {
    std::vector<std::string> out;
    for (const adapters::StrategyRecord& r : db_engine_->load_strategies()) {
        if (r.state == adapters::StrategyState::Removed) {
            out.push_back(r.name);
        }
    }
    return out;
}

void Engine::restore_strategy(const std::string& name) {
    const std::vector<adapters::StrategyRecord> records = db_engine_->load_strategies();
    const auto it = std::ranges::find_if(records, [&](const adapters::StrategyRecord& r) {
        return r.name == name && r.state == adapters::StrategyState::Removed;
    });
    if (it == records.end()) {
        throw std::invalid_argument("No removed strategy " + name);
    }
    on_event_thread([&] {
        option_strategy_engine()->restore_strategy(it->class_name, it->portfolio, it->setting,
                                                   name);
        rebook({name});
        db_engine_->set_strategy_state(name, adapters::StrategyState::Added);
        on_positions_changed();
    });
}

void Engine::halt(const std::string& reason) {
    on_event_thread([&] {
        risk_engine_->halt(reason.empty() ? "by the operator" : reason, now());
        save_risk();
        write_log("Trading halted: " + risk_engine_->halt_reason() +
                      ". Strategies stopped, working orders cancelled; only closing orders go",
                  utilities::ERROR);
        for (const std::string& name : option_strategy_engine_->get_strategy_names()) {
            const strategy::OptionStrategyTemplate* s = option_strategy_engine_->get_strategy(name);
            if (s != nullptr && s->started()) {
                option_strategy_engine_->stop_strategy(name);
                save_strategy_state(name);
            }
        }
        for (const model::OrderData& order : execution_engine_->get_all_active_orders()) {
            cancel_order(order.create_cancel_request());
        }
    });
}

void Engine::clear_halt() {
    on_event_thread([&] {
        if (!risk_engine_->halted()) {
            return;
        }
        risk_engine_->clear_halt();
        save_risk();
        write_log("Trading halt cleared: strategies may start again", utilities::WARNING);
    });
}

void Engine::set_risk_settings(const std::unordered_map<std::string, double>& values) {
    on_event_thread([&] {
        risk_engine_->set(values);
        save_risk();
        std::string set;
        for (const auto& [name, value] : values) {
            set += std::format("{}{}={}", set.empty() ? "" : ", ", name, value);
        }
        write_log("Risk limits set: " + set, utilities::INFO);
    });
}

auto Engine::book_manually(const std::string& strategy_name, const std::string& symbol,
                           model::Direction direction, double volume, double price,
                           const std::string& reason) -> std::string {
    return on_event_thread([&] {
        if (option_strategy_engine_->get_strategy(strategy_name) == nullptr) {
            throw std::invalid_argument("No strategy " + strategy_name);
        }
        if (get_contract(symbol) == nullptr) {
            throw std::invalid_argument("No contract " + symbol);
        }
        if (!(volume > 0.0) || !(price >= 0.0)) {
            throw std::invalid_argument("A fill needs a volume above 0 and a price of 0 or more");
        }
        if (reason.empty()) {
            throw std::invalid_argument("Say why it is booked by hand");
        }
        const model::DateTime at = now();
        const std::string id =
            std::format("manual-{}-{}-{}", strategy_name, symbol,
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            at.time_since_epoch())
                            .count());
        model::OrderData order;
        order.symbol = symbol;
        order.orderid = id;
        order.type = model::OrderType::MARKET;
        order.direction = direction;
        order.price = price;
        order.volume = volume;
        order.traded = volume;
        order.status = model::Status::ALLTRADED;
        order.datetime = at;
        order.reference = "manual: " + reason;
        model::TradeData fill;
        fill.symbol = symbol;
        fill.orderid = id;
        fill.tradeid = id;
        fill.direction = direction;
        fill.price = price;
        fill.volume = volume;
        fill.datetime = at;
        write_log(std::format("{}: booked by hand: {} {} {} at {} ({})", strategy_name,
                              model::to_string(direction), volume, symbol, price, reason),
                  utilities::WARNING);
        const std::array fills{fill};
        book_filled_order(strategy_name, order, fills);
        return id;
    });
}

void Engine::restore_risk() {
    std::unordered_map<std::string, double> limits;
    std::string halted;
    std::string reason;
    std::string halted_at;
    for (const auto& [name, value] : db_engine_->load_settings()) {
        if (name.starts_with("risk.limit.")) {
            try {
                limits[name.substr(std::string_view("risk.limit.").size())] = std::stod(value);
            } catch (const std::exception&) {
                write_log(std::format("Saved risk limit {} is not a number: {}", name, value),
                          utilities::ERROR);
            }
        } else if (name == "risk.halted") {
            halted = value;
        } else if (name == "risk.halt_reason") {
            reason = value;
        } else if (name == "risk.halted_at") {
            halted_at = value;
        }
    }
    try {
        risk_engine_->set(limits);
    } catch (const std::exception& e) {
        write_log(std::format("Saved risk limits not used: {}", e.what()), utilities::ERROR);
    }
    if (halted == "1") {
        long long seconds = 0;
        try {
            seconds = std::stoll(halted_at);
        } catch (const std::exception&) {
        }
        risk_engine_->halt(reason, model::DateTime{} + std::chrono::seconds(seconds));
        write_log("Trading is halted (" + reason + "), as saved: clear the halt to trade",
                  utilities::WARNING);
    }
}

void Engine::save_risk() {
    for (const auto& [name, value] : risk_engine_->settings()) {
        db_engine_->save_setting("risk.limit." + name, std::format("{}", value));
    }
    db_engine_->save_setting("risk.halted", risk_engine_->halted() ? "1" : "0");
    db_engine_->save_setting("risk.halt_reason", risk_engine_->halt_reason());
    db_engine_->save_setting(
        "risk.halted_at",
        std::to_string(std::chrono::duration_cast<std::chrono::seconds>(
                           risk_engine_->halted_at().time_since_epoch())
                           .count()));
}

auto Engine::saved_orders(const std::string& strategy, std::size_t latest)
    -> std::vector<adapters::SavedRecord> {
    return db_engine_->load_orders(
        strategy.empty() ? std::vector<std::string>{} : std::vector{strategy}, latest);
}

auto Engine::saved_trades(const std::string& strategy, std::size_t latest)
    -> std::vector<adapters::SavedRecord> {
    return db_engine_->load_trades(
        strategy.empty() ? std::vector<std::string>{} : std::vector{strategy}, latest);
}

auto Engine::strategies_with_records() -> std::vector<std::string> {
    return db_engine_->strategies_with_records();
}

auto Engine::saved_contracts() -> std::vector<model::ContractData> {
    std::vector<model::ContractData> out = db_engine_->load_option_contract_data();
    std::vector<model::ContractData> equities = db_engine_->load_equity_contract_data();
    out.insert(out.end(), std::make_move_iterator(equities.begin()),
               std::make_move_iterator(equities.end()));
    return out;
}

auto Engine::wipe_database(const std::string& what)
    -> std::vector<std::pair<std::string, int>> {
    if (what == "contracts") {
        return db_engine_->delete_contracts();
    }
    if (what != "trading") {
        throw std::invalid_argument(std::format("Cannot wipe {}: trading or contracts", what));
    }
    // On the event thread: no order or fill is queued between the check and the wipe.
    return on_event_thread([&] {
        core::OptionStrategyEngine* se = option_strategy_engine();
        for (const std::string& name : se->get_strategy_names()) {
            const model::StrategyHolding* holding = position_engine_->find_holding(name);
            if ((holding != nullptr && !holding->is_flat()) || se->has_active_order(name)) {
                throw std::runtime_error(std::format(
                    "{} holds positions or has orders working: its record is kept", name));
            }
        }
        return db_engine_->delete_orders_and_trades();
    });
}

auto Engine::reconcile_positions() -> std::vector<core::PositionCheck> {
    return reconcile_positions(query_account());
}

auto Engine::reconcile_positions(const adapters::BrokerAccount& account)
    -> std::vector<core::PositionCheck> {
    std::vector<core::PositionCheck> checks = check_positions(account);
    on_event_thread([&] { position_engine_->record_broker_check(checks); });
    return checks;
}

auto Engine::check_positions(const adapters::BrokerAccount& account)
    -> std::vector<core::PositionCheck> {
    std::map<std::string, double> broker;
    for (const adapters::AccountPosition& p : account.positions) {
        broker[p.symbol] += p.quantity;
    }
    return on_event_thread([&] {
        return position_engine_->check_positions(broker, *portfolio_structure_);
    });
}

auto Engine::positions_check() -> std::pair<std::string, std::string> {
    return on_event_thread([this] { return position_engine_->broker_check(); });
}

auto Engine::unsettled() -> std::unordered_map<std::string, std::vector<std::string>> {
    return on_event_thread(
        [this]() -> std::unordered_map<std::string, std::vector<std::string>> {
            return unsettled_options(); // a copy for the caller's thread
        });
}

auto Engine::settle_expired(const std::string& portfolio, std::chrono::year_month_day expiry,
                            double price) -> int {
    return on_event_thread([&] { return settle_expired(portfolio, expiry, price, now()); });
}

auto Engine::get_trade(const std::string& tradeid) -> model::TradeData* {
    return execution_engine_->get_trade(tradeid);
}

// ---- The market data ----

void Engine::subscribe_chains(const std::string& strategy_name,
                              std::span<const std::string> chains) {
    if (market_data_) {
        market_data_->subscribe_chains(strategy_name, chains);
    }
}

void Engine::unsubscribe_chains(const std::string& strategy_name) {
    if (market_data_) {
        market_data_->unsubscribe_chains(strategy_name);
    }
}

auto Engine::is_strategy_name_used(const std::string& strategy_name) -> bool {
    return db_engine_->is_strategy_name_used(strategy_name);
}

void Engine::on_positions_changed() {
    if (!market_data_) {
        return;
    }
    std::vector<std::string> held = position_engine_->held_options();
    if (held == held_options_) {
        return; // the same options held: the streams stay as they are
    }
    held_options_ = std::move(held);
    market_data_->hold(held_options_);
}

void Engine::put_event(const model::Event& e) { event_engine_->put_event(e); }

void Engine::put_snapshot(const model::PortfolioSnapshot& snapshot) {
    event_engine_->put_snapshot(snapshot);
}

auto Engine::acquire_snapshot() -> model::PortfolioSnapshot* {
    return event_engine_ ? event_engine_->acquire_snapshot() : nullptr;
}

auto Engine::acquire_order() -> model::OrderData* {
    return event_engine_ ? event_engine_->acquire_order() : nullptr;
}

auto Engine::acquire_trade() -> model::TradeData* {
    return event_engine_ ? event_engine_->acquire_trade() : nullptr;
}

auto Engine::pop_log_for_stream(utilities::LogData& out, int timeout_ms) -> bool {
    return log_engine() ? log_engine()->pop_log_for_stream(out, timeout_ms) : false;
}

} // namespace core
