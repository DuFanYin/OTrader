#pragma once

/** Strategy: the base every strategy derives from. */

#include "model/constant.hpp"
#include "model/order_builder.hpp"
#include "model/portfolio.hpp"
#include "model/utility.hpp"
#include "strategy/strategy_context.hpp"
#include "model/parameters.hpp"
#include "utilities/log_level.hpp"
#include <chrono>
#include <optional>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace strategy {

/** What a strategy's Setting declares its parameters with. */
using model::Param;

/**
 * Base of every strategy: what it does at each step (the on_ callbacks it overrides), and the means
 * to do it (session clock, chain selection, orders, holding, parameters), all through its
 * StrategyContext. A strategy declares a Setting of Params read with read_setting, inherits this
 * constructor, and registers with REGISTER_STRATEGY in its .cpp. Its host (core::StrategyEngine)
 * creates it and calls the callbacks: when, and in which state, is the host's.
 */
class Strategy {
  public:
    explicit Strategy(StrategyContext& context) : context_(context) {}
    virtual ~Strategy() = default;
    Strategy(const Strategy&) = delete;
    Strategy& operator=(const Strategy&) = delete;

    /** The parameters every strategy has: timer_trigger. */
    static std::vector<model::Parameter> base_parameters();
    /** Its on_timer every this many bars: how often its host calls it. */
    [[nodiscard]] int timer_trigger() const { return base_setting_.timer_trigger; }

    // ---- Callbacks
    /** As it is inited, before the day's on_day. */
    virtual void on_init() {}
    /** A trading day's contracts are in (at init, then on each roll, before the first bar). Choose
     * the day's chains here: yesterday's may be gone. Send no orders: live, the market may not be
     * open yet. */
    virtual void on_day() {}
    /** As it stops: close what it holds, if it should. */
    virtual void on_stop() {}
    /** While running, once a bar (a minute of event time; every timer_trigger bars), in backtest
     * and live. */
    virtual void on_timer() = 0;
    /** While running: quotes, forwards, IV and Greeks are fresh; live on each changing quote
     * batch, backtest once a bar before its timer. */
    virtual void on_snapshot() {}
    /** An order it sent changed, a fill of one came. Default: logged. */
    virtual void on_order(const model::Order& order);
    virtual void on_fill(const model::Fill& fill);

    // ---- What it is
    const std::string& strategy_name() const { return context_.record().name; }
    const model::Portfolio* portfolio() const { return &context_.portfolio(); }
    const std::string& portfolio_name() const { return context_.portfolio().name; }
    const model::StrategyHolding* holding() const { return &context_.holding(); }
    /** None until the portfolio's underlying is loaded. */
    std::optional<model::Underlying> underlying() const { return portfolio()->underlying(); }

    /** Event time of the portfolio's last quotes (backtest: the bar's time); use it, not timer
     * counts, for the clock. */
    model::DateTime now() const { return portfolio()->datetime(); }
    /** The contracts' trading day: what days to expiry count from. */
    std::chrono::year_month_day day() const {
        return std::chrono::year_month_day{
            std::chrono::floor<std::chrono::days>(portfolio()->trading_day())};
    }
    /** Minutes since the 09:30 New York open: the bar ending 09:31 is 1, the close's 390. */
    int minute() const { return model::minutes_since_open(now()); }

    // ---- Chains
    /** Chains find_chain finds and live streams quotes for. */
    void subscribe_chains(std::span<const std::string> chain_symbols);
    std::optional<model::Chain> find_chain(const std::string& chain_symbol) const;
    /** The nearest chain expiring min_dte to max_dte days out, subscribed in place of those
     * before; empty when there is none. */
    std::string select_chain(int min_dte, int max_dte);
    /** The chain's `type` option `steps` strikes from its at-the-money strike (up for positive),
     * or none: no at-the-money strike yet, a place off the chain's strikes, or none listed. */
    std::optional<model::Option> option_at(const model::Chain& chain, model::OptionType type,
                                           int steps = 0) const;

    // ---- Orders: one kind, units of legs. A leg is a contract (an option, the underlying, the
    // hedge instrument) with its side and how many a unit; one leg at ratio 1 is that contract's
    // own order, more a combo.
    /** A leg of the contract named `symbol`. */
    model::Leg leg(const std::string& symbol, model::Side side, int ratio = 1) const;
    /** The legs of combo_type (STRADDLE, SPREAD, ...) over its roles ("call", "put", ...) and
     * the options in them (by symbol), bought (BUY) or sold; none (logged) when a role is
     * missing. */
    std::vector<model::Leg> combo_legs(model::ComboType combo_type,
                                       const std::unordered_map<std::string, std::string>& roles,
                                       model::Side side) const;
    /** `quantity` units of `legs` at market, or at a limit `price`: the net a unit, paid positive,
     * received negative. The order id; empty when not sent (no legs, a contract not listed, or
     * refused). */
    std::string send_order(std::span<const model::Leg> legs, double price = 0.0,
                           double quantity = 1.0,
                           model::OrderType order_type = model::OrderType::MARKET,
                           model::ComboType combo_type = model::ComboType::CUSTOM);
    void register_hedging(int timer_trigger = 5, int delta_target = 0, int delta_range = 0);
    void unregister_hedging();
    /** At market: the options in one order per combo they were opened in, then the shares. */
    void close_all_strategy_positions();
    /** At market, in one order: one option as its own order, several as a combo. */
    void close_options(std::span<const model::OptionPosition> options);
    bool has_active_order() const;
    /** Holds an option, alone or in a combo. */
    bool holds_options() const;
    // ---- Values by day (data/daily, prepared outside the engine), as of the day before
    /** The series' last value dated before today. */
    std::optional<double> daily(std::string_view series) const;
    /** Its last n values before today, oldest first (fewer when it has fewer). */
    std::vector<double> daily_history(std::string_view series, std::size_t n) const;
    /** Trading days from today to the series' next date (0: today); -1 when it has none. */
    int trading_days_until(std::string_view series) const;

    // ---- Features (engine/quant) of the values by day; a chain's own are model::Chain's
    /** Annualised, of the last n daily returns of a series of closes. */
    std::optional<double> realized_vol(std::string_view closes, int n) const;
    /** The share of the series' last n values at or below `value` (an IV's rank: the series of
     * the ATM IV at each close, value the ATM IV now). */
    std::optional<double> rank_in(std::string_view series, double value, int n) const;

    /** "Today" below is its trading day (day()), which on_day already sees, not now()'s date.
     * It has a fill today, from its holding: true after a restart too. */
    bool traded_today() const;
    /** The minute of its latest fill today; -1 when it has none. */
    int last_fill_minute() const;
    /** The minute its latest order today was sent; -1 when it sent none. */
    int last_order_minute() const;

    /** It stops: in error until inited again (StrategyContext::set_error). */
    void set_error(const std::string& msg);
    void write_log(const std::string& msg, utilities::LogLevel level = utilities::INFO) const;

  protected:
    /** Its Setting from its record's; use as `const Setting setting_ = read_setting<Setting>();`. */
    template <typename Setting> Setting read_setting() const {
        return model::read<Setting>(context_.record().setting);
    }

  private:
    struct BaseSetting {
        Param<int> timer_trigger{"timer_trigger", 1, 1, 390}; // on_timer every N bars
    };

    StrategyContext& context_;
    const BaseSetting base_setting_ = read_setting<BaseSetting>();
    std::unordered_set<std::string> subscribed_chains_; // looked up in the portfolio: a day's roll
                                                        // may drop a chain
};

} // namespace strategy
