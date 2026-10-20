#pragma once

/**
 * Backtests, on an adapters::Replay: the market, each day's contracts rolled into it and each
 * bar's quotes applied to it once for the backtests on it. A Backtest is an engine on a
 * replay, a day at a time, and measures each day. run_backtests runs trials over many days: the
 * days loaded ahead, the trials on threads, those of a thread on one replay.
 */

#include "model/daily_table.hpp"
#include "adapters/gateway/backtest_gateway.hpp"
#include "adapters/market_data/historical_data.hpp"
#include "core/main.hpp"
#include "model/object.hpp"
#include "model/portfolios.hpp"
#include "utilities/log_level.hpp"
#include <cstddef>
#include <chrono>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace backtest {

// ---- Results

/** One bar: the holding's P&L and Greeks. */
struct Metric {
    double pnl = 0.0;
    double delta = 0.0;
    double theta = 0.0;
    double gamma = 0.0;
};

/** One day of a trial, as its engine measured it. Holdings carry over, so a day's P&L includes the
 * overnight move. */
struct DailyResult {
    std::string file_path;
    size_t file_index = 0;
    bool load_failed = false; ///< the day did not load; why in errors
    model::DateTime start_time{};
    model::DateTime end_time{};
    int processed_timesteps = 0; ///< the bars run
    int64_t total_rows = 0;
    double final_pnl = 0.0; ///< the run's P&L at the day's close, from its start
    double daily_pnl = 0.0;
    double daily_fees = 0.0;
    int total_orders = 0;
    double max_delta = 0.0;
    double max_gamma = 0.0;
    double max_theta = 0.0;
    double max_margin = 0.0; ///< the most margin its holding took at a bar (core::RiskEngine::margin)
    double underlying_open = 0.0;  ///< the underlying's first mid of the day (0: none quoted)
    double underlying_close = 0.0; ///< and its last
    model::HoldingSummary summary; ///< the holding at the close; pnl_* by source are the day's
    bool flat_at_close = true;       ///< nothing held at the close
    std::vector<Metric> metrics;     ///< each bar's, when kept (Backtest::keep_metrics)
    std::vector<model::Fill> fills; ///< contract fills (a combo's are its legs'), by time
    std::vector<std::string> errors;
    std::vector<utilities::LogRecord> log; ///< the day's log records, when the trial asks for them
    std::map<std::string, double, std::less<>> values; ///< what its strategy wrote (write_value)
};

/** A position from flat to flat again, a bar at a time: what a per-trade report measures. */
struct RoundTrip {
    model::DateTime opened{}; ///< the bar its first fill came in
    model::DateTime closed{}; ///< the bar it was flat again; none while still open at the run's end
    int days = 1;             ///< the trading days it spans
    double net = 0.0;         ///< P&L after fees
    double fees = 0.0;
    double max_loss = std::numeric_limits<double>::quiet_NaN(); ///< StrategyHolding::max_loss, opened
    double entry_cost = 0.0; ///< USD its opening orders paid over the mids
    double exit_cost = 0.0;  ///< USD its other option orders paid over the mids
    double settled = 0.0;    ///< USD its options settled for in cash (share-settled: delivered)
    int share_fills = 0;     ///< the underlying's or hedge's fills, a settlement's delivery included
    model::HoldingSummary attribution; ///< pnl_* by source over it
};

/** A trial's run over its days: what it measured, each day and each round trip; what the days
 * add up to is the reader's to sum. */
struct BacktestRunSummary {
    std::string strategy_name;
    std::string portfolio_name;          ///< of the days it ran; empty when none did
    std::vector<DailyResult> daily_results;
    std::vector<RoundTrip> round_trips; ///< by when they opened; the last may still be open
    std::vector<std::string> errors;    ///< each once, in the order met; why it stopped last
    double duration_seconds = 0.0;
};

// ---- One engine, a day at a time

/**
 * core::Main with the data's clock, BacktestGateway as the venue (orders fill at the next bar's quotes),
 * BacktestDatabase, and its event loop not started (events processed as they are put), on a
 * replay. A day runs bar by bar: the quotes, the bar before's orders filled at them, then the
 * timer. Each day is measured from the close before.
 */
class Backtest {
  public:
    explicit Backtest(adapters::Replay& replay);
    ~Backtest();

    Backtest(const Backtest&) = delete;
    Backtest& operator=(const Backtest&) = delete;

    core::Main& main() { return main_; }
    const core::Main& main() const { return main_; }

    adapters::BacktestGateway* backtest_gateway() { return backtest_gateway_; }
    /** Throws for a negative fee or delay. */
    void configure_execution(const adapters::BacktestGateway::Execution& execution);
    /** Each bar's Metric in the day's result. */
    void keep_metrics(bool keep) { keep_metrics_ = keep; }
    /** BacktestTrial::restart_minute. */
    void restart_at(int minute) { restart_minute_ = minute; }

    /** A day loaded (shared, not copied) and rolled into the replay already, `dropped` the chains
     * its roll dropped; the chains its strategy subscribes, holds or orders are loaded in it as
     * the day runs. Holdings and orders carry over from the day before. */
    adapters::HistoricalData* use_day(const adapters::LoadedDay& day,
                                      std::span<const std::string> dropped = {});
    adapters::HistoricalData* historical_data() const { return day_.get(); }

    /** The one strategy added (a backtest runs one); none before. */
    const model::StrategyRecord* strategy() const;

    /** The day through the one strategy added: false when it cannot run (end_day's result says
     * why); else run_bar for each bar the replay applied, then end_day. Orders still working at
     * the close are cancelled, as IB day orders are. */
    bool begin_day();
    void run_bar(std::size_t i);
    DailyResult end_day();
    /** After a day of many: its orders and fills, taken into its result, let go of. */
    void forget_finished_orders();
    /** The round trips so far, the one still open last. */
    std::vector<RoundTrip> round_trips() const;

  private:
    /** The chains its strategy wants (subscribed, held, ordered) loaded in the day. */
    void load_chains_wanted();
    void cancel_active_orders();
    /** After the bar: the day's extremes, and its Metric when kept. */
    void measure(std::size_t i, const model::Frame& bar);
    /** The bar's new fills costed against the mids, a round trip opened or closed. */
    void track_round_trip(const model::Frame& bar);

    adapters::Replay& replay_;
    adapters::BacktestGateway* backtest_gateway_ = nullptr; // core::Main's venue; set as main_ is built
    core::Main main_;
    adapters::LoadedDay day_;
    const model::Portfolio* portfolio_ = nullptr; // the day's, on the replay
    const model::StrategyRecord* strategy_ = nullptr;        // from begin_day
    const model::StrategyHolding* holding_ = nullptr; // the strategy's, from begin_day
    bool used_a_day_ = false;
    bool keep_metrics_ = false;
    int restart_minute_ = 0;
    std::uint64_t wanted_changes_ = 0; // core::StrategyEngine::wanted_changes() when last loaded
    // The day being run, measured from the close before.
    DailyResult result_;
    bool running_ = false;
    std::size_t earlier_orders_ = 0;
    model::HoldingSummary before_; // the holding at the close before
    double fees_before_ = 0.0;
    std::size_t fills_reported_ = 0; // the books' fills (in booking order) a day's result took
    std::size_t fills_costed_ = 0;   // and a round trip took in
    std::vector<RoundTrip> round_trips_;
    std::optional<RoundTrip> open_trip_;
    std::unordered_set<std::string> opening_orders_; // the open trip's first orders
    model::HoldingSummary last_bar_;                 // the holding after the bar before
    double fees_at_last_bar_ = 0.0;
    model::DateTime last_bar_time_{};
};

// ---- Many trials over many days

/** One trial of an experiment. fills keeps each day's fills (DailyResult::fills; a batch of many
 * leaves them out, the most of what its days would hold); minute_metrics each bar's P&L and
 * Greeks (for charts). */
struct BacktestTrial {
    std::string strategy_name;
    std::unordered_map<std::string, double> strategy_setting;
    /** Risk limits in place of the defaults (core::RiskEngine::set; throws on a bad one). */
    std::unordered_map<std::string, double> risk_setting;
    adapters::BacktestGateway::Execution execution;
    bool fills = false;
    bool minute_metrics = true;
    bool log = false; ///< keep each day's log records (DailyResult::log), at the data's time
    std::vector<std::chrono::year_month_day> log_days; ///< with log: only these days' (none: all)
    /** Each day at this minute the strategy is made again, as a restart makes it
     * (core::StrategyEngine::make_again); 0: never. */
    int restart_minute = 0;
    /** Values by day its strategy reads (data/daily), the same for every trial; must outlive the
     * run. nullptr: none. */
    const model::DailyTable* daily = nullptr;
};

/** Runs every trial over parquet_files in date order, each on an engine of its own, as live trades
 * them: expiries settle and working orders cancel at the close, holdings carry overnight. Days
 * load ahead in parallel, up to `days_ahead` at a time, once for all the trials (through cache_dir if given); a day's chains load, and are
 * priced, as its engines subscribe, hold or order them, each once; no trials: only the days'
 * contracts and minutes are cached. The trials run on up to `threads` threads (0: one per core),
 * those of a thread on one replay. A trial stops with an error if it holds something across a
 * missing or unloadable day, or a contract the next day does not quote; the others go on. One
 * summary per trial, in order. */
std::vector<BacktestRunSummary>
run_backtests(const std::vector<std::string>& parquet_files, std::span<const BacktestTrial> trials,
              double risk_free_rate, const std::string& iv_price_mode,
              utilities::LogLevel log_level, const std::string& cache_dir = {},
              std::size_t threads = 0, std::size_t days_ahead = 3);

} // namespace backtest
