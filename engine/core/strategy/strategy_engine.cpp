#include "core/strategy/strategy_engine.hpp"
#include "core/bookkeeping/bookkeeping_engine.hpp"
#include "model/order_builder.hpp"
#include "model/portfolios.hpp"
#include "model/symbol.hpp"
#include "strategy/strategy_context.hpp"
#include "strategy/strategy_registry.hpp"
#include "strategy/strategy.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <format>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace core {

namespace {
/** What a hedge order's reference starts with, its strategy's name after: how a hedge is told from
 * the strategy's own orders. */
constexpr std::string_view kHedgeReference = "Hedge_";

/** Sorted, each once. */
void make_set(std::vector<std::string>& values) {
    std::ranges::sort(values);
    values.erase(std::ranges::unique(values).begin(), values.end());
}
} // namespace

/** The engine as one hosted strategy sees it. */
class StrategyEngine::Context final : public strategy::StrategyContext {
  public:
    Context(StrategyEngine& engine, Hosted& hosted, const model::StrategyHolding& holding)
        : engine_(engine), hosted_(hosted), holding_(holding) {}

    const model::StrategyRecord& record() const override { return hosted_.record; }
    const model::Portfolio& portfolio() const override { return *hosted_.portfolio; }
    const model::StrategyHolding& holding() const override { return holding_; }
    bool has_active_order() const override {
        return engine_.bookkeeping_.has_active_order(hosted_.record.name);
    }
    const model::DailyTable& daily() const override { return *engine_.daily_; }
    std::string send_order(const model::OrderRequest& request) override {
        return engine_.callbacks_.send_order(hosted_.record, request);
    }
    void register_hedging(int timer_trigger, int delta_target, int delta_range) override {
        engine_.register_hedging(hosted_, {.timer_trigger = timer_trigger,
                                           .delta_target = delta_target,
                                           .delta_range = delta_range});
    }
    void unregister_hedging() override { hosted_.hedging.reset(); }
    void subscribe_chains(std::span<const std::string> chains) override {
        engine_.subscribe(hosted_, chains);
    }
    bool subscribed(std::string_view chain) const override {
        return std::ranges::contains(hosted_.chains, chain);
    }
    void write_log(const std::string& msg, utilities::LogLevel level) const override {
        engine_.write_log(hosted_, msg, level);
    }
    void set_error(const std::string& msg) override { engine_.set_error(hosted_, msg); }
    void write_value(std::string_view name, double value) override {
        hosted_.values.insert_or_assign(std::string(name), value);
    }

  private:
    StrategyEngine& engine_;
    Hosted& hosted_;
    const model::StrategyHolding& holding_;
};

StrategyEngine::StrategyEngine(utilities::Logger* logger,
                               const model::Portfolios& portfolios, BookkeepingEngine& book,
                               Callbacks callbacks)
    : logger_(logger), log_(logger, "Strategy"), portfolios_(portfolios), bookkeeping_(book),
      callbacks_(std::move(callbacks)) {}

StrategyEngine::~StrategyEngine() = default;

// ---- Hosting

auto StrategyEngine::add_strategy(model::StrategyRecord asked,
                                  const std::function<bool(const std::string&)>& used_before)
    -> std::string {
    // History is rebooked by name on restart, so a name is never reused.
    const auto taken = [&](const std::string& name) {
        return hosted(name) != nullptr || (used_before && used_before(name));
    };
    std::string& name = asked.name;
    if (name.empty()) {
        const std::string base = asked.class_name + "_" + asked.portfolio;
        name = base;
        for (int n = 2; taken(name); ++n) {
            name = std::format("{}_{}", base, n);
        }
    } else if (hosted(name) != nullptr) {
        throw std::runtime_error(
            std::format("Strategy '{}' already exists; remove it or add under another name", name));
    } else if (used_before && used_before(name)) {
        throw std::invalid_argument(
            std::format("{} was used before, and its orders and fills are kept under that name:"
                        " restore it if it was removed, or choose another name",
                        name));
    }
    std::string added = name;
    create_strategy(std::move(asked));
    return added;
}

void StrategyEngine::restore_strategy(const model::StrategyRecord& record) {
    if (hosted(record.name) != nullptr) {
        throw std::runtime_error(std::format("Strategy '{}' already exists", record.name));
    }
    create_strategy(record);
}

void StrategyEngine::create_strategy(model::StrategyRecord record) {
    const model::Portfolio* portfolio = portfolios_.find_portfolio(record.portfolio);
    if (portfolio == nullptr) {
        throw std::runtime_error("Portfolio not found: " + record.portfolio);
    }
    // Throws on an unknown class or a setting the class does not declare or allow.
    record.setting = strategy::StrategyRegistry::resolve_setting(record.class_name, record.setting);
    record.state = model::StrategyState::Added;
    if (approved_ && !approved_(record)) {
        throw std::runtime_error(std::format(
            "{} with this setting and code is not approved for this account", record.class_name));
    }
    auto h = std::make_unique<Hosted>();
    h->record = std::move(record);
    h->portfolio = portfolio;
    h->context = std::make_unique<Context>(*this, *h, bookkeeping_.ensure_holding(h->record.name));
    h->strategy = strategy::StrategyRegistry::create(*h->context);
    h->timer = model::BarGate(h->strategy->timer_trigger());
    write_log(*h, "Strategy " + h->record.name + " created for portfolio " + h->record.portfolio,
              utilities::INFO);
    hosted_.push_back(std::move(h));
    enter(*hosted_.back(), model::StrategyState::Added);
}

auto StrategyEngine::remove_strategy(const std::string& strategy_name) -> bool {
    Hosted* h = hosted(strategy_name);
    if (h == nullptr) {
        return false;
    }
    if (const model::StrategyHolding* holding = bookkeeping_.find_holding(strategy_name);
        holding != nullptr && !holding->is_flat()) {
        throw std::runtime_error(strategy_name + " holds positions: close them first");
    }
    if (bookkeeping_.has_active_order(strategy_name)) {
        throw std::runtime_error(strategy_name + " has orders working: cancel them first");
    }
    const std::string name = strategy_name; // strategy_name may be the strategy's own
    if (h->record.state == model::StrategyState::Running) {
        stop_strategy(name);
    }
    enter(*h, model::StrategyState::Removed);
    std::erase_if(hosted_, [h](const std::unique_ptr<Hosted>& each) { return each.get() == h; });
    bookkeeping_.remove_strategy(name);
    wanted_changed();
    return true;
}

void StrategyEngine::close() {
    // No on_stop: closing the engine is not stopping its strategies. A strategy's on_stop may close
    // its positions; on a restart the strategy resumes with them as saved.
    hosted_.clear();
}

// ---- Its states

void StrategyEngine::enter(Hosted& h, model::StrategyState state) {
    h.record.state = state;
    if (callbacks_.state_changed) {
        callbacks_.state_changed(h.record);
    }
}

void StrategyEngine::init_strategy(const std::string& strategy_name) {
    Hosted& h = hosted_named(strategy_name);
    if (h.record.state != model::StrategyState::Added && h.record.state != model::StrategyState::Error) {
        throw std::logic_error(strategy_name + " is inited already");
    }
    h.error.clear();
    h.day = {};
    enter(h, model::StrategyState::Inited);
    try {
        h.strategy->on_init();
        day(h);
    } catch (const std::exception& e) {
        set_error(h, std::string("init threw: ") + e.what());
        throw;
    }
}

void StrategyEngine::start_strategy(const std::string& strategy_name) {
    Hosted& h = hosted_named(strategy_name);
    switch (h.record.state) {
    case model::StrategyState::Inited:
        enter(h, model::StrategyState::Running);
        return;
    case model::StrategyState::Added:
    case model::StrategyState::Removed:
        throw std::logic_error(strategy_name + " is not inited: init it first");
    case model::StrategyState::Running:
        throw std::logic_error(strategy_name + " is running already");
    case model::StrategyState::Error:
        throw std::logic_error(strategy_name + " is in error (" + h.error +
                               "): init it again first");
    }
}

void StrategyEngine::stop_strategy(const std::string& strategy_name) {
    Hosted& h = hosted_named(strategy_name);
    if (h.record.state != model::StrategyState::Running) {
        throw std::logic_error(strategy_name + " is not running");
    }
    enter(h, model::StrategyState::Inited);
    try {
        h.strategy->on_stop();
    } catch (const std::exception& e) {
        // Not passed on: a halt stops every strategy, and one that throws must not keep the others
        // running.
        set_error(h, std::format("on_stop threw: {}: stopped, its positions stay", e.what()));
    }
}

void StrategyEngine::make_again(const std::string& strategy_name) {
    Hosted& h = hosted_named(strategy_name);
    h.strategy = strategy::StrategyRegistry::create(*h.context);
    if (h.record.state == model::StrategyState::Inited || h.record.state == model::StrategyState::Running) {
        h.day = {};
        try {
            h.strategy->on_init();
            day(h);
        } catch (const std::exception& e) {
            set_error(h, std::string("init threw: ") + e.what());
        }
    }
}

void StrategyEngine::set_error(const std::string& strategy_name, const std::string& msg) {
    set_error(hosted_named(strategy_name), msg);
}

void StrategyEngine::set_error(Hosted& h, const std::string& msg) {
    if (h.record.state == model::StrategyState::Error) {
        return; // the first cause stays
    }
    h.error = msg;
    write_log(h, msg, utilities::ERROR);
    enter(h, model::StrategyState::Error);
}

void StrategyEngine::day(Hosted& h) {
    const std::chrono::year_month_day today{
        std::chrono::floor<std::chrono::days>(h.portfolio->trading_day())};
    if ((h.record.state != model::StrategyState::Inited && h.record.state != model::StrategyState::Running) ||
        today == h.day) {
        return; // the same day's contracts again (a sync during the day): nothing new to choose
    }
    h.day = today;
    h.strategy->on_day();
}

// ---- What it hosts

auto StrategyEngine::hosted(const std::string& strategy_name) const -> Hosted* {
    // A handful of strategies: found by comparing names, in the order added.
    const auto it = std::ranges::find(
        hosted_, strategy_name,
        [](const std::unique_ptr<Hosted>& h) -> const std::string& { return h->record.name; });
    return it != hosted_.end() ? it->get() : nullptr;
}

auto StrategyEngine::hosted_named(const std::string& strategy_name) const -> Hosted& {
    Hosted* h = hosted(strategy_name);
    if (h == nullptr) {
        throw std::invalid_argument("No strategy " + strategy_name);
    }
    return *h;
}

void StrategyEngine::write_log(const Hosted& h, const std::string& msg,
                               utilities::LogLevel level) const {
    if (logger_ != nullptr) {
        logger_->log(level, h.record.name, msg);
    }
}

auto StrategyEngine::find_record(const std::string& strategy_name) const -> const model::StrategyRecord* {
    const Hosted* h = hosted(strategy_name);
    return h != nullptr ? &h->record : nullptr;
}

auto StrategyEngine::error(const std::string& strategy_name) const -> std::string {
    const Hosted* h = hosted(strategy_name);
    return h != nullptr ? h->error : std::string{};
}

auto StrategyEngine::take_values(const std::string& strategy_name)
    -> std::map<std::string, double, std::less<>> {
    Hosted* h = hosted(strategy_name);
    return h != nullptr ? std::exchange(h->values, {}) : std::map<std::string, double, std::less<>>{};
}

auto StrategyEngine::errors() const -> std::vector<std::pair<std::string, std::string>> {
    std::vector<std::pair<std::string, std::string>> out;
    for (const auto& h : hosted_) {
        if (h->record.state == model::StrategyState::Error) {
            out.emplace_back(h->record.name, h->error);
        }
    }
    return out;
}

auto StrategyEngine::find_strategy(const std::string& strategy_name) const
    -> strategy::Strategy* {
    const Hosted* h = hosted(strategy_name);
    return h != nullptr ? h->strategy.get() : nullptr;
}

// ---- What they want streamed

void StrategyEngine::subscribe_chains(const std::string& strategy_name,
                                      std::span<const std::string> chains) {
    if (Hosted* h = hosted(strategy_name)) {
        subscribe(*h, chains);
    }
}

void StrategyEngine::subscribe(Hosted& h, std::span<const std::string> chains) {
    h.chains.assign(chains.begin(), chains.end());
    wanted_changed();
}

void StrategyEngine::note_ordered(const model::OrderRequest& request) {
    bool added = false;
    for (const model::Leg& leg : request.legs) {
        if (model::parse_option_symbol(leg.symbol) && ordered_options_.insert(leg.symbol).second) {
            added = true;
        }
    }
    if (added) {
        wanted_changed();
    }
}

void StrategyEngine::clear_ordered() {
    ordered_options_.clear();
    wanted_changed();
}

void StrategyEngine::set_held_options(std::span<const std::string> options) {
    held_options_.assign(options.begin(), options.end());
    wanted_changed();
}

void StrategyEngine::wanted_changed() {
    auto wanted = std::make_shared<Wanted>();
    for (const auto& h : hosted_) {
        wanted->subscribed.insert(wanted->subscribed.end(), h->chains.begin(), h->chains.end());
    }
    make_set(wanted->subscribed);
    wanted->options = held_options_;
    wanted->options.insert(wanted->options.end(), ordered_options_.begin(),
                           ordered_options_.end());
    make_set(wanted->options);
    wanted->chains = wanted->subscribed;
    for (const std::string& option : wanted->options) {
        if (const std::optional<model::OptionId> id = model::parse_option_symbol(option)) {
            wanted->chains.push_back(model::chain_symbol(id->root, id->expiry));
        }
    }
    make_set(wanted->chains);
    wanted_.store(std::move(wanted), std::memory_order_release);
    ++wanted_changes_;
}

// ---- Its hedging

auto StrategyEngine::hedging(const std::string& strategy_name) const -> const Hedging* {
    const Hosted* h = hosted(strategy_name);
    return h != nullptr && h->hedging ? &*h->hedging : nullptr;
}

void StrategyEngine::register_hedging(Hosted& h, const Hedging& hedging) {
    h.hedging = hedging;
    h.hedge_timer = model::BarGate(hedging.timer_trigger);
    if (!h.portfolio->hedge()) {
        write_log(h,
                  std::format("Hedges {}, which has no {} contract to hedge with: sync the "
                              "contracts",
                              h.record.portfolio,
                              h.portfolio->hedge_symbol().empty()
                                  ? std::string_view("underlying")
                                  : h.portfolio->hedge_symbol()),
                  utilities::WARNING);
    }
}

void StrategyEngine::run_hedging() {
    for (const auto& h : hosted_) {
        if (!h->hedging || !h->hedge_timer.next(h->portfolio->datetime())) {
            continue;
        }
        std::vector<model::CancelRequest> cancels;
        bookkeeping_.for_each_active_order(h->record.name, [&](const model::Order& order) {
            if (order.reference.starts_with(kHedgeReference)) {
                cancels.push_back(order.create_cancel_request());
            }
        });
        for (const model::CancelRequest& cancel : cancels) {
            callbacks_.cancel_order(cancel);
        }
        if (!cancels.empty()) {
            continue;
        }
        if (const model::StrategyHolding* holding = bookkeeping_.find_holding(h->record.name)) {
            if (std::optional<model::OrderRequest> order = hedge_order(*h, *holding)) {
                (void)callbacks_.send_order(h->record, *order);
            }
        }
    }
}

auto StrategyEngine::hedge_order(const Hosted& h, const model::StrategyHolding& holding) const
    -> std::optional<model::OrderRequest> {
    const Hedging& hedging = *h.hedging;
    const std::optional<model::Underlying> instrument = h.portfolio->hedge();
    // theo_delta: the underlying's units per unit of the instrument; 0 until it has a price.
    if (!instrument || instrument->theo_delta() <= 0.0) {
        return std::nullopt;
    }
    const double delta = holding.summary.delta;
    if (std::abs(delta - hedging.delta_target) <= hedging.delta_range) {
        return std::nullopt;
    }
    const double shares = (hedging.delta_target - delta) / instrument->theo_delta();
    // Whole units, the nearest: positions and the broker take no fraction of a share.
    const double units = std::round(std::abs(shares));
    const model::Contract* contract = h.portfolio->contract(instrument->symbol());
    if (contract == nullptr || units < 1) {
        return std::nullopt;
    }
    const model::Side side =
        shares > 0 ? model::Side::BUY : model::Side::SELL;
    const std::array leg{model::contract_leg(*contract, side)};
    model::OrderRequest req = model::order_request(leg, 0.0, units, model::OrderType::MARKET);
    req.reference = std::format("{}{}", kHedgeReference, h.record.name);
    write_log(h,
              std::format("Hedging delta {} with {} {} {}", delta, model::to_string(side),
                          units, req.symbol),
              utilities::INFO);
    return req;
}


// ---- Events

template <typename F> void StrategyEngine::timed(Hosted& h, std::string_view callback, F&& f) {
    const std::uint64_t t0 = utilities::tsc_now();
    try {
        f();
    } catch (const std::exception& e) {
        // A strategy's throw stops that strategy only, not the event thread; its positions stay.
        set_error(h, std::format("{} threw: {}: stopped, its positions stay", callback, e.what()));
    }
    const std::uint64_t ns = utilities::tsc_ns(t0, utilities::tsc_now());
    h.took.record(ns);
    if (callback_budget_ <= std::chrono::microseconds::zero()) {
        return;
    }
    if (std::chrono::nanoseconds(static_cast<std::int64_t>(ns)) <= callback_budget_) {
        h.over_in_a_row = 0;
        return;
    }
    ++h.over_in_a_row;
    const auto now = std::chrono::steady_clock::now();
    if (now - h.warned >= std::chrono::minutes(1)) {
        h.warned = now;
        log_.write_log(std::format("{} {} took {} us: over the {} us budget ({} in a row)",
                                   h.record.name, callback, ns / 1000, callback_budget_.count(),
                                   h.over_in_a_row),
                       utilities::WARNING);
    }
    if (h.over_in_a_row >= kCallbacksOverToStop) {
        set_error(h, std::format("{} callbacks in a row over the {} us budget: stopped, its "
                                 "positions stay",
                                 h.over_in_a_row, callback_budget_.count()));
    }
}

void StrategyEngine::on_order(const model::Order& order) {
    if (Hosted* h = hosted(bookkeeping_.strategy_of(order.order_id))) {
        timed(*h, "on_order", [&] { h->strategy->on_order(order); });
    }
}

void StrategyEngine::on_fill(const model::Fill& fill) {
    if (Hosted* h = hosted(bookkeeping_.strategy_of(fill.order_id))) {
        timed(*h, "on_fill", [&] { h->strategy->on_fill(fill); });
    }
}

void StrategyEngine::on_timer() {
    for (const auto& h : hosted_) {
        if (h->record.state != model::StrategyState::Running || !h->timer.next(h->portfolio->datetime())) {
            continue;
        }
        timed(*h, "on_timer", [&] { h->strategy->on_timer(); });
    }
}

void StrategyEngine::on_snapshot(std::string_view portfolio_name) {
    for (const auto& h : hosted_) {
        if (h->record.state == model::StrategyState::Running && h->record.portfolio == portfolio_name) {
            timed(*h, "on_snapshot", [&] { h->strategy->on_snapshot(); });
        }
    }
}

void StrategyEngine::on_day(const std::string& portfolio_name) {
    for (const auto& h : hosted_) {
        if (h->record.portfolio == portfolio_name) {
            timed(*h, "on_day", [&] { day(*h); });
        }
    }
}

// ---- How long callbacks take

auto StrategyEngine::callback_report() const -> std::string {
    std::string out;
    for (const auto& h : hosted_) {
        if (h->took.summary().count > 0) {
            out += (out.empty() ? "  " : "\n  ") + h->took.line(h->record.name + " callbacks");
        }
    }
    return out;
}

} // namespace core
