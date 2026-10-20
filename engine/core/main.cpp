#include "core/main.hpp"

#include "model/order_builder.hpp"
#include "model/session_calendar.hpp"
#include "model/symbol.hpp"
#include "model/utility.hpp"
#include "utilities/text.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <format>
#include <map>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <utility>
#include <variant>

namespace core {

namespace {

/** The event's payload if it carries a T, else null. */
template <typename T> auto payload(const model::Event& event) -> T* {
    const auto* slot = std::get_if<T*>(&event.data);
    return slot != nullptr ? *slot : nullptr;
}

/** For a line saying something was asked of the source: how many times, after the first. */
auto again(int times) -> std::string {
    return times > 1 ? std::format(" (again: no answer to the {} before)", times - 1)
                     : std::string{};
}

} // namespace

Main::Main(Ports ports, utilities::LogLevel log_level)
    : log_(log_level),
      portfolios_(ports.portfolios != nullptr ? *ports.portfolios : own_portfolios_),
      clock_(std::move(ports.clock)),
      database_(ports.database(&log_)),
      gateway_(ports.gateway(&log_, portfolios_)),
      market_data_(&log_, portfolios_, strategy_engine_, std::move(ports.market_data),
                   [this](const model::PortfolioSnapshot& s) { event_loop_.put_snapshot(s); }),
      broker_check_warn_only_(ports.broker_check_warn_only) {
    risk_engine_.restore_limits(database_->load_settings());
    if (gateway_->has_broker()) {
        risk_engine_.watch_broker([this] { return gateway_->is_connected(); });
    }
    read_daily_ = std::move(ports.daily);
    if (read_daily_) {
        daily_ = std::make_shared<const model::DailyTable>(read_daily_());
        use_daily(daily_.get());
    }
    market_data_.on_close([this](const std::string& underlying, std::chrono::year_month_day day,
                                 double close) {
        event_loop_.put_event(model::Event(
            model::EventType::Task,
            new model::Task([this, underlying, day, close] { settle_at(underlying, day, close); })));
    });
    market_data_.on_day_range([this](const std::string& underlying, std::chrono::year_month_day day,
                                     double open, double high, double low) {
        event_loop_.put_event(model::Event(
            model::EventType::Task, new model::Task([this, underlying, day, open, high, low] {
                restore_day(underlying, day, open, high, low);
            })));
    });
    gateway_->set_callbacks([this](const model::Order& o) { event_loop_.put_order(o); },
                            [this](const model::Fill& t) { event_loop_.put_fill(t); });
}

Main::~Main() { close(); }

void Main::write_log(const std::string& msg, utilities::LogLevel level) const {
    log_.log(level, "Main", msg);
}

auto Main::latency_report() const -> std::string {
    std::string out = event_loop_.latency_report();
    if (const std::string callbacks = strategy_engine_.callback_report(); !callbacks.empty()) {
        out += "\nStrategy callbacks (since start):\n" + callbacks;
    }
    return out;
}

// ---- Events

void Main::process_event(const model::Event& event) {
    switch (event.type) {
        using enum model::EventType;
    case Snapshot:
        if (auto* snapshot = payload<model::PortfolioSnapshot>(event)) {
            apply_snapshot(*snapshot);
        }
        break;
    case Timer:
        on_timer(now());
        break;
    case Order:
        if (const auto* order = payload<model::Order>(event)) {
            book_order(*order);
        }
        break;
    case Fill:
        if (const auto* fill = payload<model::Fill>(event)) {
            book_fill(*fill);
        }
        break;
    case Task: // run by the EventLoop
        break;
    }
}

void Main::apply_snapshot(model::PortfolioSnapshot& snapshot) {
    if (model::Portfolio* portfolio =
            portfolios_.find_portfolio(std::string(snapshot.portfolio_name))) {
        portfolio->apply(snapshot);
        snapshot_applied(portfolio->frame());
    }
}

void Main::snapshot_applied(const model::Frame& frame) {
    strategy_engine_.on_snapshot(frame.portfolio_name);
}

// ---- Booking

void Main::book_order(const model::Order& order) {
    if (const std::optional<model::Order> known =
            bookkeeping_engine_.update_order(order, now())) {
        order_changed(*known);
    }
}

void Main::order_changed(const model::Order& order) {
    if (const std::string& strategy_name =
            bookkeeping_engine_.strategy_of(order.order_id);
        !strategy_name.empty()) {
        database_->save_order(strategy_name, order);
    }
    strategy_engine_.on_order(order);
}

void Main::book_fill(const model::Fill& reported) {
    std::optional<model::Fill> stamped;
    if (reported.datetime == model::DateTime{}) { // IB's carry none: stamped as an order sent is
        stamped = reported;
        stamped->datetime = now();
    }
    const model::Fill& fill = stamped ? *stamped : reported;
    const std::string* booked = bookkeeping_engine_.book_fill(fill);
    if (booked == nullptr) {
        return; // booked already: IB repeats today's fills when the engine reconnects
    }
    const std::string& strategy_name = *booked;
    if (strategy_name.empty()) {
        // Not a strategy's (placed by hand, say): the broker check reports it.
        write_log(std::format("Fill {} of order {} ({} {}) is no strategy's: not held",
                              fill.fill_id, fill.order_id, fill.quantity, fill.symbol),
                  utilities::WARNING);
        return;
    }
    database_->save_fill(strategy_name, fill);
    if (bookkeeping_engine_.take_held_options_changed()) {
        strategy_engine_.set_held_options(bookkeeping_engine_.held_options());
    }
    strategy_engine_.on_fill(fill);
    if (const std::optional<model::Order> end = bookkeeping_engine_.take_released(fill)) {
        order_changed(*end);
    }
}

auto Main::book_filled_order(const std::string& strategy_name, const model::Order& order,
                             std::span<const model::Fill> fills) -> bool {
    // An id booked before would take these fills as its own, and drop them as repeats.
    if (bookkeeping_engine_.find_order(order.order_id) != nullptr) {
        write_log(std::format("{}: order {} is booked already: not booked again", strategy_name,
                              order.order_id),
                  utilities::ERROR);
        return false;
    }
    // Stored as sent first, as send_order does, so each fill finds its order (and combo leg).
    model::Order sent = order;
    sent.status = model::Status::SUBMITTING;
    sent.filled = 0.0;
    bookkeeping_engine_.add_order(strategy_name, std::move(sent));
    book_order(order); // held back until the fills below are booked
    for (const model::Fill& t : fills) {
        book_fill(t);
    }
    return true;
}

auto Main::settle(const std::string& strategy_name, std::chrono::year_month_day expiry,
                  double price, const model::Portfolio& portfolio) -> int {
    int booked = 0;
    for (const auto& [order, fills] :
         bookkeeping_engine_.settle_expiry(strategy_name, expiry, price, portfolio)) {
        if (!book_filled_order(strategy_name, order, fills)) {
            continue;
        }
        write_log(std::format("{}: settled {} {} at {} ({})", strategy_name, order.quantity,
                              order.symbol, order.price,
                              fills.size() > 1 ? "its legs" : order.reference),
                  utilities::INFO);
        ++booked;
    }
    return booked;
}

void Main::rebook(const std::unordered_set<std::string>& strategies) {
    // Replay in save order; knowing every fill lets IB's repeats on connect be dropped.
    const std::vector<std::string> names(strategies.begin(), strategies.end());
    std::vector<model::Order> working;
    std::size_t orders = 0;
    for (SavedOrder& saved : database_->load_orders(names)) {
        model::Order& order = saved.record;
        bookkeeping_engine_.add_order(saved.strategy_name, order);
        ++orders;
        if (order.is_active()) {
            working.push_back(std::move(order));
        }
    }
    std::size_t fills = 0;
    for (const SavedFill& saved : database_->load_fills(names)) {
        if (bookkeeping_engine_.book_fill(saved.record) != nullptr) {
            ++fills;
        }
    }
    gateway_->restore_orders(working); // they may fill: they stay theirs
    write_log(std::format("Rebooked {} orders ({} working) and {} fills", orders, working.size(),
                          fills),
              utilities::INFO);
}

// ---- Each bar

void Main::on_timer(model::DateTime now) {
    for (const model::Order& end : bookkeeping_engine_.take_overdue(now)) {
        write_log(std::format("Order {}: its end ({} filled) told after {} without all its fills"
                              " booked",
                              end.order_id, end.filled, BookkeepingEngine::kTerminalUpdateTimeout),
                  utilities::WARNING);
        order_changed(end);
    }
    for (const auto& [s, portfolio] : strategy_engine_.strategies_and_portfolios()) {
        risk_engine_.start_bar(s.name, portfolio, now);
    }
    settle_due(now);
    ask_day_ranges(now);
    flatten_expiring(now);
    strategy_engine_.on_timer();
    for (const auto& [s, portfolio] : strategy_engine_.strategies_and_portfolios()) {
        bookkeeping_engine_.value_holding(s.name, portfolio);
        risk_engine_.measure_exposure(s.name, portfolio);
    }
    if (const std::optional<std::string> why = risk_engine_.day_loss()) {
        // The strategies keep running, so they can close what they hold; nothing opens.
        risk_engine_.halt(*why, now);
        write_log("Trading halted: " + *why + ". Only closing orders go until the halt is cleared",
                  utilities::ERROR);
        save_risk();
    }
    if (risk_engine_.take_day_changed()) {
        for (const auto& [name, value] : risk_engine_.saved_day()) {
            database_->save_setting(name, value);
        }
    }
    strategy_engine_.run_hedging();
}

void Main::settle_due(model::DateTime now) {
    for (const auto& [s, portfolio] : strategy_engine_.strategies_and_portfolios()) {
        for (const auto& [expiry, price] :
             bookkeeping_engine_.expiries_to_settle(s.name, &portfolio, now)) {
            settle(s.name, expiry, price, portfolio);
        }
    }
    // What waits for a close the engine did not see (it was not running): asked of the source,
    // and settled when it answers (settle_at).
    for (const auto& [s, portfolio] : strategy_engine_.strategies_and_portfolios()) {
        for (const Asked::Key& close : bookkeeping_engine_.closes_awaited(s.name, portfolio)) {
            if (closes_asked_.due(close, now) &&
                market_data_.ask_close(close.first, close.second)) {
                const int times = closes_asked_.asked(close, now);
                write_log(std::format("Asked for {}'s close of {} to settle {}'s expired options{}",
                                      close.first, close.second, s.name, again(times)),
                          utilities::INFO);
            }
        }
    }
}

void Main::use_daily(const model::DailyTable* daily) {
    strategy_engine_.use_daily(daily);
    bookkeeping_engine_.use_daily(daily);
    if (daily == nullptr) {
        return;
    }
    // Live prices on the Treasury curve of the day before, as a backtest's day file carries it.
    const std::chrono::year_month_day today = model::kNyse.date_of(now());
    if (auto curve = model::RateCurve::treasury(*daily, today)) {
        write_log(std::format("Pricing on the Treasury curve before {}: {:.4f} at 1 month to {:.4f}"
                              " at 2 years",
                              today, curve->rates.front(), curve->rates.back()),
                  utilities::INFO);
        market_data_.use_rates(*std::move(curve));
    } else {
        write_log("No Treasury curve in the values by day: pricing at 5%", utilities::WARNING);
    }
}

void Main::settle_at(const std::string& underlying, std::chrono::year_month_day day,
                     double close) {
    for (const auto& [s, portfolio] : strategy_engine_.strategies_and_portfolios()) {
        if (portfolio.underlying_symbol() == underlying &&
            settle(s.name, day, close, portfolio) > 0) {
            write_log(std::format("{}: settled what expired {} at {}'s close, {}", s.name, day,
                                  underlying, close),
                      utilities::INFO);
        }
    }
    settle_due(now()); // what waits is known again now, not at the next quote
}

void Main::ask_day_ranges(model::DateTime now) {
    for (const auto& [s, portfolio] : strategy_engine_.strategies_and_portfolios()) {
        if (portfolio.saw_open() || portfolio.day_first_minute() < 1) {
            continue;
        }
        const Asked::Key day{std::string(portfolio.underlying_symbol()),
                             portfolio.calendar().date_of(portfolio.datetime())};
        if (ranges_asked_.due(day, now) && market_data_.ask_day_range(day.first, day.second)) {
            const int times = ranges_asked_.asked(day, now);
            write_log(std::format("{} first quoted {} minutes after the open: asked for {}'s open,"
                                  " high and low of {}{}",
                                  portfolio.name, portfolio.day_first_minute(), day.first,
                                  day.second, again(times)),
                      utilities::INFO);
        }
    }
}

auto Main::Asked::due(const Key& key, model::DateTime now) const -> bool {
    const auto found = asking_.find(key);
    return found == asking_.end() || now >= found->second.again;
}

auto Main::Asked::asked(const Key& key, model::DateTime now) -> int {
    Asking& a = asking_[key];
    if (a.times > 0) {
        a.wait = std::min(a.wait * 2, kAtMost);
    }
    a.again = now + a.wait;
    return ++a.times;
}

void Main::restore_day(const std::string& underlying, std::chrono::year_month_day day,
                       double open, double high, double low) {
    for (const auto& [s, portfolio] : strategy_engine_.strategies_and_portfolios()) {
        model::Portfolio* p = portfolios_.find_portfolio(portfolio.name);
        if (p == nullptr || p->underlying_symbol() != underlying || p->saw_open()) {
            continue;
        }
        p->restore_day(day, open, high, low);
        if (p->saw_open()) {
            write_log(std::format("{}: the day's open {}, high {}, low {} from the source",
                                  p->name, p->day_open(), p->day_high(), p->day_low()),
                      utilities::INFO);
        }
    }
}

void Main::flatten_expiring(model::DateTime now) {
    for (const auto& [s, portfolio] : strategy_engine_.strategies_and_portfolios()) {
        for (const auto& [combo, options] :
             bookkeeping_engine_.positions_to_close(s.name, portfolio, now,
                                                    risk_engine_.flatten_minutes())) {
            write_log(std::format("{}: closing {} before the close: it settles in shares today, or"
                                  " a call sold may be exercised for tomorrow's dividend",
                                  s.name, combo),
                      utilities::WARNING);
            auto order = model::closing_order(
                options, [&](const std::string& symbol) { return portfolio.contract(symbol); });
            if (!order) {
                write_log(std::format("{}: {}", s.name, order.error()), utilities::WARNING);
                continue;
            }
            order->reference = "Flatten_" + s.name; // the engine's, not the strategy's own
            (void)send_order(s, *order);
        }
    }
}

// ---- Orders

auto Main::send_order(const model::StrategyRecord& strategy, model::OrderRequest req)
    -> std::string {
    const std::string& strategy_name = strategy.name;
    if (req.reference.empty()) {
        req.reference = "Strategy_" + strategy_name;
    }
    if (!risk_engine_.allow(strategy_name, strategy.portfolio, req, now())) {
        return {};
    }
    strategy_engine_.note_ordered(req); // its quotes stream: the fill and the holding read them
    // Stored as sent (contract, legs) before any venue update, so that every fill finds its order.
    std::string order_id = gateway_->send_order(req);
    event_loop_.note_order_sent(); // live: the quote -> order latency
    if (!order_id.empty()) {
        model::Order sent = req.create_order(order_id);
        sent.datetime = now(); // as sent, until the venue reports it
        database_->save_order(strategy_name, sent);
        bookkeeping_engine_.add_order(strategy_name, std::move(sent));
    }
    return order_id;
}

void Main::cancel_order(const model::CancelRequest& req) {
    // Once an order; it stays active (and may fill) until the venue ends it.
    if (bookkeeping_engine_.cancel_sent(req.order_id)) {
        gateway_->cancel_order(req);
    }
}

// ---- The broker

void Main::connect() {
    const std::scoped_lock lock(connect_mutex_);
    gateway_->connect();
    if (!gateway_->has_broker()) {
        return;
    }
    // Catch-up fills arrive as events before the catch-up completes, so they are booked by the
    // time reconciliation runs behind them on the event thread. IB timeouts fail the request.
    try {
        gateway_->catch_up().get();
        check_with_broker();
    } catch (const std::exception& e) {
        event_loop_.on_event_thread(
            [&] { bookkeeping_engine_.record_broker_check_failed(e.what()); });
    }
}

auto Main::sync_contracts(const std::string& portfolio, int max_dte)
    -> std::vector<model::Contract> {
    // The gateway fails the sync if IB stops answering or the connection drops.
    std::vector<model::Contract> contracts = gateway_->sync_contracts(portfolio, max_dte).get();
    database_->save_contracts(contracts);
    write_log(std::format("Synced {} {} contracts", contracts.size(), portfolio), utilities::INFO);
    return contracts;
}

void Main::roll_contracts(const std::string& portfolio,
                          std::span<const model::Contract> contracts, model::DateTime day) {
    event_loop_.on_event_thread([&] {
        const std::vector<std::string> held = bookkeeping_engine_.held_options();
        const std::vector<std::string> dropped = portfolios_.replace_contracts(
            portfolio, contracts, std::unordered_set<std::string>(held.begin(), held.end()));
        if (model::Portfolio* p = portfolios_.find_portfolio(portfolio)) {
            p->set_trading_day(std::chrono::floor<std::chrono::days>(day));
            contracts_rolled(portfolio, dropped);
        }
    });
}

void Main::contracts_rolled(const std::string& portfolio, std::span<const std::string> dropped) {
    event_loop_.on_event_thread([&] {
        const model::Portfolio* p = portfolios_.find_portfolio(portfolio);
        if (p == nullptr) {
            return;
        }
        write_log(std::format("{}: the day's {} options in {} chains ({} chains gone{}{})",
                              portfolio, p->listing().size(), p->listing().chains().size(),
                              dropped.size(), dropped.empty() ? "" : ": ", utilities::join(dropped)),
                  utilities::INFO);
        strategy_engine_.on_day(portfolio);
        strategy_engine_.clear_ordered();
    });
}

auto Main::roll_day(std::span<const Synced> portfolios) -> std::vector<model::Contract> {
    const std::scoped_lock lock(roll_mutex_);
    std::vector<std::pair<std::string, std::vector<model::Contract>>> synced;
    for (const auto& [portfolio, max_dte] : portfolios) {
        // Throws: none saved, none rolled.
        synced.emplace_back(portfolio, gateway_->sync_contracts(portfolio, max_dte).get());
    }
    for (const auto& [portfolio, contracts] : synced) {
        database_->save_contracts(contracts);
        write_log(std::format("Synced {} {} contracts", contracts.size(), portfolio),
                  utilities::INFO);
    }
    const bool streaming = market_data_.running();
    market_data_.stop(); // it prices the portfolios on its own thread
    for (const auto& [portfolio, contracts] : synced) {
        roll_contracts(portfolio, contracts, now());
    }
    market_data_.load_contracts();
    if (streaming) {
        market_data_.start();
    }
    if (read_daily_) { // the closes of the day before, prepared since
        try {
            auto read = std::make_shared<const model::DailyTable>(read_daily_());
            event_loop_.on_event_thread([&] {
                daily_.swap(read);
                use_daily(daily_.get());
            });
        } catch (const std::exception& e) {
            write_log(std::string("The values by day were not read again: ") + e.what(),
                      utilities::ERROR);
        }
    }
    write_log(std::format("The day's contracts are in ({} portfolios)", synced.size()),
              utilities::INFO);
    std::vector<model::Contract> all;
    for (auto& [_, contracts] : synced) {
        all.append_range(contracts | std::views::as_rvalue);
    }
    return all;
}

// ---- Holdings and strategies

auto Main::add_strategy(const std::string& class_name, const std::string& portfolio,
                        const model::Values& setting, const std::string& strategy_name)
    -> std::string {
    return event_loop_.on_event_thread([&] {
        return strategy_engine_.add_strategy(
            {.name = strategy_name, .class_name = class_name, .portfolio = portfolio,
             .setting = setting},
            [this](const std::string& used) { return database_->is_strategy_name_used(used); });
    });
}

void Main::init_strategy(const std::string& name) {
    event_loop_.on_event_thread([&] { strategy_engine_.init_strategy(name); });
}

void Main::start_strategy(const std::string& name) {
    event_loop_.on_event_thread([&] {
        if (risk_engine_.halted()) {
            throw std::runtime_error("Trading is halted (" + risk_engine_.halt_reason() +
                                     "): clear the halt first");
        }
        if (const auto refusal = bookkeeping_engine_.start_refusal(
                name, now(), gateway_->has_broker() && !broker_check_warn_only_)) {
            throw std::runtime_error(*refusal);
        }
        strategy_engine_.start_strategy(name);
    });
}

void Main::stop_strategy(const std::string& name) {
    event_loop_.on_event_thread([&] { strategy_engine_.stop_strategy(name); });
}

void Main::remove_strategy(const std::string& name) {
    event_loop_.on_event_thread([&] {
        if (!strategy_engine_.remove_strategy(name)) {
            throw std::invalid_argument("No strategy " + name);
        }
        risk_engine_.remove_strategy(name);
    });
}

void Main::delete_strategy(const std::string& name) {
    event_loop_.on_event_thread([&] {
        if (strategy_engine_.find_record(name) != nullptr) {
            remove_strategy(name);
        }
        database_->delete_strategy(name);
    });
}

void Main::restore_strategy(const std::string& name) {
    const std::vector<model::StrategyRecord> records = database_->load_strategies();
    const auto it = std::ranges::find_if(records, [&](const model::StrategyRecord& r) {
        return r.name == name && r.state == model::StrategyState::Removed;
    });
    if (it == records.end()) {
        throw std::invalid_argument("No removed strategy " + name);
    }
    event_loop_.on_event_thread([&] {
        strategy_engine_.restore_strategy(*it);
        rebook({name});
        strategy_engine_.set_held_options(bookkeeping_engine_.held_options());
    });
}

void Main::restore() {
    std::unordered_set<std::string> loaded;
    std::vector<model::StrategyRecord> to_init;
    std::vector<std::string> in_error;
    for (const model::StrategyRecord& r : database_->load_strategies()) {
        if (r.state == model::StrategyState::Removed) {
            continue;
        }
        try {
            strategy_engine_.restore_strategy(r);
            loaded.insert(r.name);
            if (r.state == model::StrategyState::Error) {
                in_error.push_back(r.name);
            } else if (r.state != model::StrategyState::Added) {
                to_init.push_back(r);
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
    for (const model::StrategyRecord& r : to_init) { // on_init sees its holding
        try {
            strategy_engine_.init_strategy(r.name);
            model::StrategyRecord kept = *strategy_engine_.find_record(r.name);
            if (kept.state != model::StrategyState::Error) {
                // Inited by the restore: saved as it was (running, say) until the run starts it.
                kept.state = r.state;
                database_->save_strategy(kept);
            }
        } catch (const std::exception& e) {
            write_log(std::format("Cannot init {}: {}", r.name, e.what()), utilities::ERROR);
        }
    }
    for (const std::string& name : in_error) {
        strategy_engine_.set_error(name, "In error when the engine stopped: init it again");
    }
    strategy_engine_.set_held_options(bookkeeping_engine_.held_options());
    write_log(std::format("Restored {} strategies ({} inited); start them, or start the run"
                          " (resume)",
                          loaded.size(), to_init.size()),
              utilities::INFO);
}

void Main::start_strategies_that_were_running() {
    for (const model::StrategyRecord& r : database_->load_strategies()) {
        if (r.state != model::StrategyState::Running ||
            !event_loop_.on_event_thread(
                [&] { return strategy_engine_.find_record(r.name) != nullptr; })) {
            continue;
        }
        try {
            start_strategy(r.name);
            write_log("Started " + r.name + ", running when the engine stopped", utilities::INFO);
        } catch (const std::exception& e) {
            write_log(std::format("{} not started: {}", r.name, e.what()), utilities::ERROR);
        }
    }
}

// ---- Portfolios

auto Main::find_portfolio(const std::string& name) const -> const model::Portfolio* {
    return portfolios_.find_portfolio(name);
}

auto Main::find_contract(const std::string& symbol) const -> const model::Contract* {
    return portfolios_.find_contract(symbol);
}

// ---- Risk

void Main::halt(const std::string& reason) {
    event_loop_.on_event_thread([&] {
        risk_engine_.halt(reason.empty() ? "by the operator" : reason, now());
        save_risk();
        write_log("Trading halted: " + risk_engine_.halt_reason() +
                      ". Working orders cancelled, strategies stopped; only closing orders go",
                  utilities::ERROR);
        // Cancelled first: a strategy may close what it holds as it stops, and those orders go.
        std::vector<model::CancelRequest> active;
        bookkeeping_engine_.for_each_active_order(
            {}, [&](const model::Order& o) { active.push_back(o.create_cancel_request()); });
        for (const model::CancelRequest& cancel : active) {
            cancel_order(cancel);
        }
        for (const model::StrategyRecord& s : strategy_engine_.strategies()) {
            if (s.state == model::StrategyState::Running) {
                strategy_engine_.stop_strategy(s.name);
            }
        }
    });
}

void Main::clear_halt() {
    event_loop_.on_event_thread([&] {
        if (!risk_engine_.halted()) {
            return;
        }
        risk_engine_.clear_halt();
        save_risk();
        write_log("Trading halt cleared: strategies may start again", utilities::WARNING);
    });
}

void Main::set_risk_settings(const std::unordered_map<std::string, double>& values) {
    event_loop_.on_event_thread([&] {
        risk_engine_.set_limits(values);
        save_risk();
        std::string set;
        for (const auto& [name, value] : values) {
            set += std::format("{}{}={}", set.empty() ? "" : ", ", name, value);
        }
        write_log("Risk limits set: " + set, utilities::INFO);
    });
}

void Main::save_risk() {
    for (const auto& [name, value] : risk_engine_.saved_limits()) {
        database_->save_setting(name, value);
    }
}

// ---- Broker check and expiry

auto Main::check_with_broker() -> std::vector<BrokerCheckRow> {
    return check_with_broker(gateway_->query_account().get());
}

auto Main::check_with_broker(const BrokerAccount& account) -> std::vector<BrokerCheckRow> {
    std::vector<BrokerCheckRow> checks = compare_with_broker(account);
    event_loop_.on_event_thread([&] { bookkeeping_engine_.record_broker_check(checks); });
    return checks;
}

auto Main::compare_with_broker(const BrokerAccount& account) -> std::vector<BrokerCheckRow> {
    std::map<std::string, double> broker;
    for (const AccountPosition& p : account.positions) {
        broker[p.symbol] += p.quantity;
    }
    return event_loop_.on_event_thread(
        [&] { return bookkeeping_engine_.compare_with_broker(broker, portfolios_); });
}

auto Main::book_manually(const std::string& strategy_name, const std::string& symbol,
                         model::Side side, double quantity, double price,
                         const std::string& reason) -> std::string {
    return event_loop_.on_event_thread([&] {
        if (strategy_engine_.find_record(strategy_name) == nullptr) {
            throw std::invalid_argument("No strategy " + strategy_name);
        }
        if (find_contract(symbol) == nullptr && !bookkeeping_engine_.holds(strategy_name, symbol)) {
            throw std::invalid_argument(
                std::format("No contract {}, and {} holds none of it", symbol, strategy_name));
        }
        if (!(quantity > 0.0) || !(price >= 0.0)) {
            throw std::invalid_argument("A fill needs a quantity above 0 and a price of 0 or more");
        }
        if (reason.empty()) {
            throw std::invalid_argument("Say why it is booked by hand");
        }
        const model::DateTime at = now();
        const std::string id = std::format(
            "manual-{}-{}-{}", strategy_name, symbol,
            std::chrono::duration_cast<std::chrono::milliseconds>(at.time_since_epoch()).count());
        const auto [order, fill] = model::filled_order(id, symbol, side, quantity, price, at,
                                                       "manual: " + reason);
        if (!book_filled_order(strategy_name, order, std::array{fill})) {
            throw std::runtime_error(
                std::format("{} is booked already: book it again", order.order_id));
        }
        write_log(std::format("{}: booked by hand: {} {} {} at {} ({})", strategy_name,
                              model::to_string(side), quantity, symbol, price, reason),
                  utilities::WARNING);
        return order.order_id;
    });
}

auto Main::settle_expired(const std::string& portfolio, std::chrono::year_month_day expiry,
                          double price) -> int {
    return event_loop_.on_event_thread([&] {
        const model::Portfolio* p = find_portfolio(portfolio);
        if (p == nullptr) {
            throw std::invalid_argument("No portfolio " + portfolio);
        }
        if (!expiry.ok() || price <= 0.0) {
            throw std::invalid_argument("Give the expiry date and the underlying's closing price");
        }
        if (p->calendar().expiry_time(expiry) > now()) {
            throw std::invalid_argument(std::format("{:%F} has not closed yet", expiry));
        }
        int count = 0;
        for (const model::StrategyRecord& s : strategy_engine_.strategies()) {
            if (s.portfolio == portfolio) {
                count += settle(s.name, expiry, price, *p);
            }
        }
        settle_due(now()); // what waits is known again now, not at the next quote
        return count;
    });
}

// ---- Storage

auto Main::wipe_database(const std::string& what) -> std::vector<std::pair<std::string, int>> {
    if (what == "contracts") {
        return database_->delete_contracts();
    }
    if (what != "trading") {
        throw std::invalid_argument(std::format("Cannot wipe {}: trading or contracts", what));
    }
    // On the event thread: no order or fill is queued between the check and the wipe.
    return event_loop_.on_event_thread([&] {
        for (const model::StrategyRecord& s : strategy_engine_.strategies()) {
            const model::StrategyHolding* holding = bookkeeping_engine_.find_holding(s.name);
            if ((holding != nullptr && !holding->is_flat()) ||
                bookkeeping_engine_.has_active_order(s.name)) {
                throw std::runtime_error(std::format(
                    "{} holds positions or has orders working: its record is kept", s.name));
            }
        }
        return database_->delete_orders_and_fills();
    });
}

// ---- Closing

void Main::close() {
    if (closed_) {
        return;
    }
    closed_ = true;
    close_ports();
    strategy_engine_.close();
    bookkeeping_engine_.close();
}

void Main::close_ports() {
    market_data_.close();
    gateway_->close();
    event_loop_.close();
    database_->close();
}

} // namespace core
