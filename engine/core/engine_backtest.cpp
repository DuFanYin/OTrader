/** The backtest assembly of core::Engine, and its run over one day or many. */

#include "core/engine_backtest.hpp"
#include "core/engines/option_strategy.hpp"
#include "model/event.hpp"
#include "strategy/strategy_registry.hpp"
#include "strategy/template.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <limits>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>

namespace core::backtest {

namespace {

std::string ts_to_iso(adapters::Timestamp ts) {
    std::time_t t = std::chrono::system_clock::to_time_t(ts);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    std::ostringstream os;
    os << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S");
    return os.str();
}

/** A day that produced no backtest (file not loadable / never run): reason in result.errors.
 * Excluded from num_days and the return series by the entry point. */
auto failed_day(std::string const& path, size_t file_idx, std::string reason) -> DailyResult {
    DailyResult daily;
    daily.file_path = path;
    daily.file_index = file_idx;
    daily.load_failed = true;
    daily.result.errors.push_back(std::move(reason));
    return daily;
}

auto load_failure(std::string const& path, adapters::BacktestDataEngine const* de) -> std::string {
    const std::string why = de != nullptr ? de->load_error() : std::string{"no data engine"};
    return "Cannot load " + path + (why.empty() ? std::string{} : ": " + why);
}

} // namespace

MainEngine::MainEngine() {
    // Quiet from the start: a backtest's stdout is its JSON result (the ports log as they open).
    set_log_level(utilities::DISABLED);
    // The data's time: the latest snapshot's, as the timer tick after it sees it.
    clock_ = [this] { return snapshot_time_; };
    event_engine_ = std::make_unique<EventEngine>(this);
    db_engine_ = std::make_unique<adapters::DatabaseEngine>(log_engine(), ":memory:");
    auto gateway = std::make_unique<adapters::BacktestGateway>(
        *portfolio_structure_, /*fee_rate=*/0.0, /*slippage_bps=*/5.0);
    backtest_gateway_ = gateway.get();
    gateway_ = std::move(gateway);
    init_core(utilities::DISABLED);
    write_log("Main engine initialization successful", utilities::INFO);
}

MainEngine::~MainEngine() = default;

auto MainEngine::load_backtest_data(const std::string& parquet_path,
                                    const std::string& underlying_symbol)
    -> adapters::BacktestDataEngine* {
    if (!data_engine_) {
        data_engine_ =
            std::make_unique<adapters::BacktestDataEngine>(log_engine(), portfolio_structure_.get());
    }
    data_engine_->load_parquet(parquet_path, "ts_recv", underlying_symbol);
    if (data_engine_->has_data()) {
        write_log("Backtest data loaded from: " + parquet_path, utilities::INFO);
    } else {
        write_log("Backtest data NOT loaded from " + parquet_path + ": " +
                      data_engine_->load_error(),
                  utilities::ERROR);
    }
    return data_engine_.get();
}

void MainEngine::configure_execution(double fee_rate, double slippage_bps) {
    if (fee_rate < 0.0) {
        throw std::runtime_error("fee_rate must be >= 0");
    }
    backtest_gateway_->configure_execution(fee_rate, std::max(slippage_bps, 0.0));
}

void MainEngine::register_timestep_callback(TimestepCallback cb) {
    timestep_callbacks_.push_back(std::move(cb));
}

auto MainEngine::run() -> BacktestResult {
    BacktestResult result;

    adapters::BacktestDataEngine* data_engine = data_engine_.get();
    if ((data_engine == nullptr) || !data_engine->has_data()) {
        const std::string why = data_engine != nullptr ? data_engine->load_error() : std::string{};
        result.errors.emplace_back(
            why.empty() ? "No data loaded. Call load_backtest_data() first."
                        : "No data loaded: " + why);
        return result;
    }
    core::OptionStrategyEngine* strategy_engine = option_strategy_engine();
    strategy::OptionStrategyTemplate* strategy = strategy_engine->get_strategy();
    if (strategy == nullptr) {
        result.errors.emplace_back("No strategy added. Call add_strategy() first.");
        return result;
    }
    if (!strategy->started()) {
        init_strategy(strategy->strategy_name());
        start_strategy(strategy->strategy_name());
    }
    data_engine->price_precomputed_snapshots(); // with the settings the run is given
    result.strategy_name = strategy_engine->get_strategy_class_name(strategy->strategy_name());
    result.portfolio_name = strategy->portfolio_name();

    double max_delta = 0.0;
    double max_gamma = 0.0;
    double max_theta = 0.0;
    double peak_pnl = 0.0;
    double max_drawdown = 0.0;

    adapters::Timestamp start_time = std::chrono::system_clock::now();
    adapters::Timestamp end_time = start_time;
    int step_count = 0;
    int64_t total_rows = 0;

    data_engine->iter_timesteps(
        [&](adapters::Timestamp ts, adapters::TimestepFrameColumnar const& frame) -> bool {
            if (step_count == 0) {
                start_time = ts;
            }
            end_time = ts;
            // Snapshot(step_count) = end-of-bar for this minute; portfolio gets bar's BBO.
            model::PortfolioSnapshot* snap = acquire_snapshot();
            if (snap != nullptr) {
                *snap = data_engine->get_precomputed_snapshot(step_count);
                put_event(model::Event(model::EventType::Snapshot, snap));
            }
            total_rows += frame.num_rows;

            // The orders sent at the bar before fill at this one's quotes.
            backtest_gateway_->execute_pending_orders();

            // Timer: strategy runs, may send orders
            put_event(model::Event(model::EventType::Timer));

            auto* holding = strategy_engine->get_strategy_holding();
            if (holding) {
                const double pnl = holding->summary.pnl;
                max_delta = std::max(std::abs(holding->summary.delta), max_delta);
                max_gamma = std::max(std::abs(holding->summary.gamma), max_gamma);
                max_theta = std::max(std::abs(holding->summary.theta), max_theta);

                // Peak PnL, drawdown
                peak_pnl = step_count == 0 ? pnl : std::max(pnl, peak_pnl);
                max_drawdown = std::max(peak_pnl - pnl, max_drawdown);
            }

            for (auto const& cb : timestep_callbacks_) {
                cb(step_count + 1, ts);
            }
            step_count++;
            result.processed_timesteps = step_count;
            return true;
        });

    result.start_time = start_time;
    result.end_time = end_time;
    result.total_timesteps = step_count;
    result.processed_timesteps = step_count;
    result.total_frames = static_cast<int64_t>(step_count);
    result.total_rows = total_rows;
    auto* holding = strategy_engine->get_strategy_holding();
    result.final_pnl = (holding != nullptr) ? holding->summary.pnl : 0.0;
    result.max_delta = max_delta;
    result.max_gamma = max_gamma;
    result.max_theta = max_theta;
    result.max_drawdown = max_drawdown;
    result.total_orders = static_cast<int>(execution_engine_->get_all_orders().size());
    result.trades = execution_engine_->get_all_trades(); // contracts' fills: a combo's, its legs
    std::ranges::stable_sort(result.trades, {}, [](const model::TradeData& t) {
        return t.datetime;
    });
    if (!strategy->error_msg().empty()) {
        result.errors.push_back(strategy->error_msg());
    }

    return result;
}

BacktestRunSummary
run_backtest_multi(const std::vector<std::string>& parquet_files, const std::string& strategy_name,
                   double fee_rate, double slippage_bps, double risk_free_rate,
                   const std::string& iv_price_mode, utilities::LogLevel log_level,
                   const std::unordered_map<std::string, double>& strategy_setting,
                   const std::unordered_map<std::string, double>& risk_setting) {
    BacktestRunSummary summary;
    if (parquet_files.empty() || strategy_name.empty()) {
        return summary;
    }
    // An unknown class or a bad setting would fail every day alike: report it once, up front.
    (void)strategy::StrategyRegistry::resolve_setting(strategy_name, strategy_setting);

    std::vector<DailyResult>& daily_results = summary.daily_results;
    daily_results.resize(parquet_files.size());
    std::vector<double>& daily_returns = summary.daily_returns;
    daily_returns.resize(parquet_files.size());

    // One day on an engine of its own; a day that cannot run is recorded as failed (an exception
    // escaping a worker thread would end the process).
    const auto run_day = [&](size_t file_idx) -> DailyResult {
        const std::string& path = parquet_files[file_idx];
        MainEngine engine;
        engine.configure_execution(fee_rate, slippage_bps);
        engine.set_log_level(log_level);
        std::vector<Metric> file_metrics;
        file_metrics.reserve(400);
        engine.register_timestep_callback([&](int timestep, adapters::Timestamp ts) {
            Metric m;
            m.timestep = timestep;
            m.timestamp = ts_to_iso(ts);
            m.fees = engine.backtest_gateway()->cumulative_fees();
            if (auto* holding = engine.option_strategy_engine()->get_strategy_holding()) {
                m.pnl = holding->summary.pnl;
                m.delta = holding->summary.delta;
                m.gamma = holding->summary.gamma;
                m.theta = holding->summary.theta;
            }
            file_metrics.push_back(m);
        });
        adapters::BacktestDataEngine* de = engine.load_backtest_data(path);
        if (de == nullptr || !de->has_data()) {
            return failed_day(path, file_idx, load_failure(path, de));
        }
        try {
            engine.add_strategy(strategy_name, de->portfolio_name(), strategy_setting);
            engine.risk_engine()->set(risk_setting);
        } catch (const std::exception& e) { // e.g. a setting the class does not allow
            return failed_day(path, file_idx, std::string("Cannot add strategy: ") + e.what());
        }
        de->set_risk_free_rate(risk_free_rate);
        de->set_iv_price_mode(iv_price_mode);
        DailyResult daily;
        daily.file_path = path;
        daily.result = engine.run();
        daily.daily_pnl = daily.result.final_pnl;
        daily.daily_fees = engine.backtest_gateway()->cumulative_fees();
        daily.file_index = file_idx;
        daily.file_metrics = std::move(file_metrics);
        if (const auto* holding = engine.option_strategy_engine()->get_strategy_holding()) {
            daily.summary = holding->summary;
        }
        return daily;
    };
    const auto record = [&](DailyResult daily) {
        const size_t i = daily.file_index;
        daily_returns[i] = daily.load_failed ? std::numeric_limits<double>::quiet_NaN()
                                             : daily.daily_pnl - daily.daily_fees;
        daily_results[i] = std::move(daily);
    };

    const auto overall_start_time = std::chrono::system_clock::now();
    if (parquet_files.size() == 1) {
        record(run_day(0));
    } else {
        // Four engines at a time, each taking the next day.
        std::mutex results_mutex;
        std::size_t next = 0;
        const auto worker = [&] {
            for (;;) {
                size_t file_idx = 0;
                {
                    std::scoped_lock lock(results_mutex);
                    if (next == parquet_files.size()) {
                        return;
                    }
                    file_idx = next++;
                }
                DailyResult daily = run_day(file_idx);
                std::scoped_lock lock(results_mutex);
                record(std::move(daily));
            }
        };
        constexpr int kEngines = 4;
        std::vector<std::thread> threads;
        threads.reserve(kEngines);
        for (int i = 0; i < kEngines; ++i) {
            threads.emplace_back(worker);
        }
        for (std::thread& t : threads) {
            t.join();
        }
    }
    const auto overall_end_time = std::chrono::system_clock::now();

    // The days' metrics, one series in file order.
    std::vector<Metric>& metrics = summary.metrics;
    int base = 0;
    for (DailyResult& daily : daily_results) {
        for (Metric& m : daily.file_metrics) {
            m.timestep += base;
            metrics.push_back(std::move(m));
        }
        base += static_cast<int>(daily.file_metrics.size());
        daily.file_metrics.clear();
    }

    auto duration_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(overall_end_time - overall_start_time)
            .count();
    summary.duration_ms = duration_ms;
    summary.duration_seconds = duration_ms / 1000.0;

    // Aggregate results across all files
    BacktestResult aggregated_result;
    aggregated_result.strategy_name = strategy_name;
    aggregated_result.portfolio_name =
        daily_results.empty() ? std::string{} : daily_results.front().result.portfolio_name;

    std::vector<DailyResult> sorted_daily_results;
    sorted_daily_results.reserve(daily_results.size());
    for (const auto& daily : daily_results) {
        if (!daily.file_path.empty()) {
            sorted_daily_results.push_back(daily);
        }
    }
    if (!sorted_daily_results.empty()) {
        aggregated_result.start_time = sorted_daily_results.front().result.start_time;
        aggregated_result.end_time = sorted_daily_results.back().result.end_time;
    }

    for (const auto& daily : sorted_daily_results) {
        aggregated_result.processed_timesteps += daily.result.processed_timesteps;
        aggregated_result.total_timesteps += daily.result.total_timesteps;
        aggregated_result.total_frames += daily.result.total_frames;
        aggregated_result.total_rows += daily.result.total_rows;
        aggregated_result.final_pnl += daily.daily_pnl;
        aggregated_result.total_orders += daily.result.total_orders;
        aggregated_result.max_delta = std::max(aggregated_result.max_delta, daily.result.max_delta);
        aggregated_result.max_gamma = std::max(aggregated_result.max_gamma, daily.result.max_gamma);
        aggregated_result.max_theta = std::max(aggregated_result.max_theta, daily.result.max_theta);
        if (!daily.result.errors.empty()) {
            aggregated_result.errors.insert(aggregated_result.errors.end(),
                                            daily.result.errors.begin(), daily.result.errors.end());
        }
    }

    summary.aggregated_result = aggregated_result;
    return summary;
}

} // namespace core::backtest
