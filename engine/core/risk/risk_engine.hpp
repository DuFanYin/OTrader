#pragma once

/**
 * RiskEngine: pre-trade check on every outgoing order (backtest too), and the trading halt. Default
 * limits leave a backtest unconstrained; live sets its own, persisted. While halted only reducing
 * orders go out; the halt persists across restarts. Each bar it marks every strategy's exposure
 * (Greeks in USD, margin) and the day's P&L: a strategy past its daily loss trades only to close
 * for the rest of the New York day, and all of them past the global one halt. State only, event
 * thread.
 */

#include "model/daily_table.hpp"
#include "model/object.hpp"
#include "model/portfolios.hpp"
#include "model/parameters.hpp"
#include "utilities/log_source.hpp"
#include "core/bookkeeping/bookkeeping_engine.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <chrono>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace core {

class RiskEngine {
  public:
    static constexpr std::size_t kRecentRefusals = 50;

    /** The limits, by name:
     *  - max_option_order / max_share_order: contracts (each leg: the order's quantity times its
     *    ratio) / shares in one order;
     *  - max_option_position / max_share_position: what a strategy may hold of one contract /
     *    of shares, its active orders counted, after the order;
     *  - max_order_premium: USD an order may pay for options (0: no limit);
     *  - price_band: a limit price at most this fraction of the mid away from it, or within
     *    price_band_floor dollars (0: not checked);
     *  - market_max_spread: a market order needs a two-sided quote whose spread is at most this
     *    fraction of the mid (0: not checked);
     *  - short_options: 0 refuses an order that would leave the strategy, on an expiry, with more
     *    calls (or puts) sold than bought: a sold leg must be covered by one bought, as in a
     *    spread, so every loss has a floor;
     *  - flatten_minutes: options settled in shares (SPY's) and expiring today are closed this many
     *    minutes before the close, and none is bought after (0: off);
     *  - max_strategy_daily_loss / max_daily_loss: USD a strategy / all of them may lose in a New
     *    York day; past it, only closing orders for that strategy / a halt (0: off);
     *  - max_strategy_delta_usd, _gamma_usd, _vega_usd and max_total_...: a strategy's / all
     *    strategies' delta in USD (delta times the underlying), gamma in USD (the change in that
     *    delta for a 1% move) and vega in USD a vol point; an order that takes one past its limit
     *    and further from zero is refused (0: off);
     *  - max_strategy_positions: options a strategy holds at once, its active orders counted;
     *  - max_margin: USD of margin a strategy's holding takes, by margin(), after the order;
     *  - max_quote_age_seconds: no opening order on quotes older than this (0: off). Nor while
     *    the broker is not connected (watch_broker). */
    struct Limits {
        // The defaults leave a backtest as it was.
        model::Param<int> max_option_order{"max_option_order", 100, 1, 1'000'000};
        model::Param<int> max_share_order{"max_share_order", 10'000, 1, 100'000'000};
        model::Param<int> max_option_position{"max_option_position", 500, 1, 10'000'000};
        model::Param<int> max_share_position{"max_share_position", 100'000, 1, 1'000'000'000};
        model::Param<double> max_order_premium{"max_order_premium", 0, 0, 1e9};
        model::Param<double> price_band{"price_band", 0, 0, 10};
        model::Param<double> price_band_floor{"price_band_floor", 0.1, 0, 1000};
        model::Param<double> market_max_spread{"market_max_spread", 0, 0, 10};
        model::Param<int> short_options{"short_options", 1, 0, 1};
        model::Param<int> flatten_minutes{"flatten_minutes", 0, 0, 390};
        model::Param<double> max_strategy_daily_loss{"max_strategy_daily_loss", 0, 0, 1e9};
        model::Param<double> max_daily_loss{"max_daily_loss", 0, 0, 1e9};
        model::Param<double> max_strategy_delta_usd{"max_strategy_delta_usd", 0, 0, 1e10};
        model::Param<double> max_strategy_gamma_usd{"max_strategy_gamma_usd", 0, 0, 1e10};
        model::Param<double> max_strategy_vega_usd{"max_strategy_vega_usd", 0, 0, 1e10};
        model::Param<double> max_total_delta_usd{"max_total_delta_usd", 0, 0, 1e10};
        model::Param<double> max_total_gamma_usd{"max_total_gamma_usd", 0, 0, 1e10};
        model::Param<double> max_total_vega_usd{"max_total_vega_usd", 0, 0, 1e10};
        model::Param<int> max_strategy_positions{"max_strategy_positions", 0, 0, 10'000};
        model::Param<double> max_margin{"max_margin", 0, 0, 1e10};
        model::Param<int> max_quote_age_seconds{"max_quote_age_seconds", 0, 0, 86'400};
    };
    /** The limits as parameters: name, default, range. */
    static std::span<const model::Parameter> parameters();

    RiskEngine(utilities::Logger* logger, const model::Portfolios& portfolios,
               const BookkeepingEngine& book);

    /** The refusal reason, or nullopt. Empty strategy_name: holds nothing; empty portfolio: any. */
    std::optional<std::string> refusal(const std::string& strategy_name,
                                       const std::string& portfolio, const model::OrderRequest& req,
                                       model::DateTime now);
    /** refusal(), logging, counting and keeping a refusal. */
    bool allow(const std::string& strategy_name, const std::string& portfolio,
               const model::OrderRequest& req, model::DateTime now);

    /** 0 for an unknown name. */
    [[nodiscard]] double limit(std::string_view name) const;
    /** All or nothing (model::resolve over the limits as they are): throws std::invalid_argument
     * on an unknown name or out-of-range value. */
    void set_limits(const std::unordered_map<std::string, double>& values);

    /** The limits, the halt and saved_day(), as settings to save. */
    [[nodiscard]] std::vector<std::pair<std::string, std::string>> saved_limits() const;
    /** The New York day and each strategy's P&L at its start: what changes as the day turns. */
    [[nodiscard]] std::vector<std::pair<std::string, std::string>> saved_day() const;
    /** Ignores other names; an unreadable value is logged and the default kept. */
    void restore_limits(const std::unordered_map<std::string, std::string>& saved);

    /** While it says false, only closing orders go (live: the broker's connection). */
    void watch_broker(std::function<bool()> connected) { broker_connected_ = std::move(connected); }

    /** At each bar, first: a new New York day starts every strategy's day P&L from where it is.
     * On a day the market is closed the day's losses stop nothing. */
    void start_bar(model::DateTime now);
    /** After the strategy's metrics are updated: its exposure, and the day's loss checked. */
    void measure_exposure(const std::string& strategy_name, const model::Portfolio& portfolio);
    /** Its exposure leaves the totals. Its day's start and stop stay: removed and restored the
     * same day, it is where it was against its daily loss. */
    void remove_strategy(const std::string& strategy_name) { exposure_.erase(strategy_name); }
    /** Why trading should halt now (max_daily_loss), or nullopt. */
    [[nodiscard]] std::optional<std::string> day_loss() const;
    /** True once after a new day started: the day's starts are to be saved. */
    bool take_day_changed() { return std::exchange(day_changed_, false); }
    /** The strategies trading only to close for the rest of the day, and why. */
    [[nodiscard]] const std::unordered_map<std::string, std::string>& day_stops() const {
        return day_stops_;
    }
    /** USD the strategy's holding takes as margin, as last marked. */
    [[nodiscard]] double margin(const std::string& strategy_name) const;
    /** A holding's margin, estimated: on each expiry, the most its options can lose at expiry
     * (none for options bought: they are paid for), plus 20% of the underlying for each call sold
     * uncovered; shares, half their value. */
    static double margin(std::span<const std::pair<std::string_view, double>> holding,
                         const model::Portfolio& portfolio);

    void halt(const std::string& reason, model::DateTime at);
    void clear_halt();
    [[nodiscard]] bool halted() const { return halted_; }
    [[nodiscard]] const std::string& halt_reason() const { return halt_reason_; }
    [[nodiscard]] model::DateTime halted_at() const { return halted_at_; }

    /** recent_refusals: "HH:MM:SS strategy symbol: reason". */
    [[nodiscard]] std::uint64_t refused() const { return refused_; }
    [[nodiscard]] const std::deque<std::string>& recent_refusals() const {
        return recent_refusals_;
    }

    /** A share-settled option expiring today, within flatten_minutes of the close. */
    [[nodiscard]] bool in_flatten_window(const std::string& option_symbol,
                                         const model::Portfolio* portfolio,
                                         model::DateTime now) const;
    /** Where ex-dividend days are (the series <underlying>_ex_dividend); it must outlive the
     * engine. nullptr: none. */
    void use_daily(const model::DailyTable* daily) { daily_ = daily; }
    /** The options to close now, by the combo each was opened in: those of the window above, and,
     * the day before an ex-dividend day of a share-settled underlying, every combo with a call
     * sold (it may be exercised early for the dividend), within flatten_minutes of the close.
     * Skips those with an active order (the close sent a tick ago) so closes are not stacked. */
    [[nodiscard]] std::map<std::string, std::vector<model::OptionPosition>>
    positions_to_flatten(const std::string& strategy_name, const model::Portfolio& portfolio,
                         model::DateTime now) const;

  private:
    /** The New York day found last is one the market opens (or none is found yet). */
    [[nodiscard]] bool trading_day() const;
    /** A strategy's exposure as marked: signed USD; and its pnl when the day began. */
    struct Exposure {
        double delta = 0.0;
        double gamma = 0.0;
        double vega = 0.0;
        double margin = 0.0;
        std::optional<double> day_start;
    };
    /** One contract of a strategy's book: what it holds, what its active orders would still trade
     * and what the order checked trades, signed. */
    struct Line {
        const std::string* symbol = nullptr; // the order's, the holding's or an active order's
        double held = 0.0;
        double active = 0.0;
        double change = 0.0;
    };
    /** The order's contracts first, in its order (the count returned); with `whole`, then every
     * other contract the strategy holds (`holding`, its) or has active orders in. None of the
     * strategy's without one. */
    std::size_t book(const std::string& strategy_name, const model::StrategyHolding* holding,
                     const model::OrderRequest* req, bool whole, std::vector<Line>& out) const;
    /** Each contract's quantity, held or (after) with the active orders and the order; into
     * quantities_. */
    std::span<const std::pair<std::string_view, double>> quantities(std::span<const Line> book,
                                                                     bool after);
    /** The order's exposure checks (Greeks, positions, margin) over lines_, built whole by
     * refusal(); its first `ordered` lines the order's. */
    std::optional<std::string> exposure_refusal(const std::string& strategy_name,
                                                const model::Portfolio& portfolio,
                                                std::size_t ordered);
    /** On some expiry, more calls or puts sold than bought: what, or nullopt. */
    static std::optional<std::string>
    uncovered(std::span<const std::pair<std::string_view, double>> holding);
    const model::Portfolio* portfolio_of(const std::string& portfolio,
                                             const std::string& symbol) const;
    /** bid, ask, mid; nullopt when unquoted. */
    std::optional<std::array<double, 3>> quote(const model::Portfolio* portfolio,
                                               const std::string& symbol) const;

    utilities::LogSource log_;
    const model::Portfolios& portfolios_;
    const BookkeepingEngine& bookkeeping_;
    const model::DailyTable* daily_ = nullptr; // not owned
    model::Values limit_values_; // every limit, by name
    Limits limits_;              // the same, read
    // Reused from one check to the next.
    std::vector<Line> lines_;
    std::vector<std::pair<std::string_view, double>> quantities_;
    bool halted_ = false;
    std::string halt_reason_;
    model::DateTime halted_at_{};
    std::uint64_t refused_ = 0;
    std::deque<std::string> recent_refusals_;
    std::function<bool()> broker_connected_;
    std::chrono::year_month_day day_{};
    model::DateTime day_begins_{}; // the New York day the last bar was in, as instants
    model::DateTime day_ends_{};
    bool day_changed_ = false;
    std::unordered_map<std::string, std::string> day_stops_;
    std::unordered_map<std::string, Exposure> exposure_; // by strategy
};

} // namespace core
