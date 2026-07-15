#include "engine_backtest.hpp"
#include "../../core/engine_option_strategy.hpp"
#include "../../strategy/template.hpp"
#include "../../utilities/event.hpp"
#include "engine_main.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <stop_token>
#include <thread>
#include <utility>

namespace backtest {

namespace {

std::string ts_to_iso(Timestamp ts) {
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

} // namespace

BacktestEngine::BacktestEngine()
    : main_engine_(std::make_unique<MainEngine>()),
      gateway_(main_engine_.get(), /*fee_rate=*/0.0, /*slippage_bps=*/5.0) {
    // Orders queued for next timestep
    main_engine_->set_order_executor([this](const utilities::OrderRequest& req) -> std::string {
        return this->submit_order(req);
    });
    main_engine_->set_cancel_executor([this](const utilities::CancelRequest& req) {
        // Cancel open order so it won't be executed in later timesteps.
        open_orders_.erase(req.orderid);
    });
}

void BacktestEngine::configure_execution(double fee_rate, double slippage_bps) {
    if (fee_rate < 0.0) {
        throw std::runtime_error("fee_rate must be >= 0");
    }
    slippage_bps = std::max(slippage_bps, 0.0);
    fee_rate_ = fee_rate;
    slippage_bps_ = slippage_bps;
    gateway_.configure_execution(fee_rate_, slippage_bps_);
}

auto BacktestEngine::submit_order(const utilities::OrderRequest& req) -> std::string {
    order_counter_++;
    std::string orderid = "backtest_order_" + std::to_string(order_counter_);
    open_orders_.emplace(orderid, req);
    return orderid;
}

bool BacktestEngine::execute_order_impl(const utilities::OrderRequest& req,
                                        const std::string& orderid) {
    return gateway_.execute_order(req, orderid, trade_counter_, cumulative_fees_);
}

void BacktestEngine::execute_pending_orders() {
    std::vector<std::string> filled;
    filled.reserve(open_orders_.size());
    for (const auto& [orderid, req] : open_orders_) {
        if (execute_order_impl(req, orderid)) {
            filled.push_back(orderid);
        }
    }
    for (const auto& oid : filled) {
        open_orders_.erase(oid);
    }
}

void BacktestEngine::load_backtest_data(std::string const& parquet_path,
                                        std::string const& underlying_symbol) {
    if (main_engine_) {
        main_engine_->load_backtest_data(parquet_path, underlying_symbol);
    }
}

void BacktestEngine::add_strategy(std::string const& strategy_name,
                                  std::unordered_map<std::string, double> const& setting) {
    strategy_name_ = strategy_name;
    strategy_setting_ = setting;
    // Portfolio "backtest"
    std::string portfolio_name = "backtest";
    if (main_engine_ && (main_engine_->option_strategy_engine() != nullptr)) {
        main_engine_->option_strategy_engine()->add_strategy(strategy_name, portfolio_name,
                                                             setting);
    }
}

void BacktestEngine::register_timestep_callback(TimestepCallback cb) {
    timestep_callbacks_.push_back(std::move(cb));
}

auto BacktestEngine::get_current_state() const -> std::unordered_map<std::string, double> {
    std::unordered_map<std::string, double> m;
    m["pnl"] = current_pnl_;
    m["delta"] = current_delta_;
    if (main_engine_ && (main_engine_->option_strategy_engine() != nullptr)) {
        auto* holding = main_engine_->option_strategy_engine()->get_strategy_holding();
        if (holding != nullptr) {
            m["pnl"] = holding->summary.pnl;
            m["delta"] = holding->summary.delta;
        }
    }
    return m;
}

auto BacktestEngine::run() -> BacktestResult {
    BacktestResult result;
    result.strategy_name = strategy_name_;
    result.portfolio_name = "backtest";
    result.errors = errors_;

    BacktestDataEngine* data_engine = main_engine_ ? main_engine_->get_data_engine() : nullptr;
    if ((data_engine == nullptr) || !data_engine->has_data()) {
        result.errors.emplace_back("No data loaded. Call main_engine.load_backtest_data() first.");
        return result;
    }
    core::OptionStrategyEngine* strategy_engine =
        main_engine_ ? main_engine_->option_strategy_engine() : nullptr;
    if ((strategy_engine == nullptr) || (strategy_engine->get_strategy() == nullptr)) {
        result.errors.emplace_back("No strategy added. Call add_strategy() first.");
        return result;
    }
    auto* strategy = strategy_engine->get_strategy();
    if ((strategy != nullptr) && !strategy->inited()) {
        strategy->on_init();
        strategy->on_start();
    }
    if (strategy != nullptr) {
        result.portfolio_name = strategy->portfolio_name();
    }

    current_timestep_ = 0;
    current_pnl_ = 0.0;
    current_delta_ = 0.0;
    cumulative_fees_ = 0.0;
    max_delta_ = 0.0;
    max_gamma_ = 0.0;
    max_theta_ = 0.0;

    Timestamp start_time = std::chrono::system_clock::now();
    Timestamp end_time = start_time;
    int step_count = 0;
    int64_t total_rows = 0;

    data_engine->iter_timesteps(
        [this, &result, &start_time, &end_time, &step_count, &total_rows, data_engine,
         strategy_engine](Timestamp ts, TimestepFrameColumnar const& frame) -> bool {
            if (step_count == 0) {
                start_time = ts;
            }
            end_time = ts;
            // Snapshot(step_count) = end-of-bar for this minute; portfolio gets bar's BBO.
            utilities::PortfolioSnapshot* snap = main_engine_->acquire_snapshot();
            if (snap != nullptr) {
                *snap = data_engine->get_precomputed_snapshot(step_count);
                main_engine_->put_event(utilities::Event(utilities::EventType::Snapshot, snap));
            }
            current_timestep_ = step_count + 1;
            total_rows += frame.num_rows;

            // Execute pending (next-bar)
            execute_pending_orders();

            // Timer: strategy runs, may send orders
            main_engine_->put_event(utilities::Event(utilities::EventType::Timer));

            auto* holding = strategy_engine->get_strategy_holding();
            if (holding) {
                current_pnl_ = holding->summary.pnl;
                current_delta_ = holding->summary.delta;
                max_delta_ = std::max(std::abs(holding->summary.delta), max_delta_);
                max_gamma_ = std::max(std::abs(holding->summary.gamma), max_gamma_);
                max_theta_ = std::max(std::abs(holding->summary.theta), max_theta_);

                // Peak PnL, drawdown
                if (step_count == 0) {
                    peak_pnl_ = current_pnl_;
                } else {
                    peak_pnl_ = std::max(current_pnl_, peak_pnl_);
                }
                double drawdown = peak_pnl_ - current_pnl_;
                max_drawdown_ = std::max(drawdown, max_drawdown_);
            }

            for (auto const& cb : timestep_callbacks_) {
                cb(current_timestep_, ts);
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
    result.max_delta = max_delta_;
    result.max_gamma = max_gamma_;
    result.max_theta = max_theta_;
    result.max_drawdown = max_drawdown_;
    result.total_orders = static_cast<int>(strategy_engine->get_all_orders().size());
    result.errors = errors_;
    if ((strategy_engine->get_strategy() != nullptr) &&
        !strategy_engine->get_strategy()->error_msg().empty()) {
        result.errors.push_back(strategy_engine->get_strategy()->error_msg());
    }

    return result;
}

void BacktestEngine::reset() {
    // Reset state
    current_timestep_ = 0;
    current_pnl_ = 0.0;
    current_delta_ = 0.0;
    max_delta_ = 0.0;
    max_gamma_ = 0.0;
    max_theta_ = 0.0;
    peak_pnl_ = 0.0;
    max_drawdown_ = 0.0;
    total_orders_ = 0;
    cumulative_fees_ = 0.0;
    errors_.clear();
    timestep_callbacks_.clear();
    strategy_name_.clear();
    strategy_setting_.clear();
    open_orders_.clear();
    order_counter_ = 0;
    trade_counter_ = 0;

    // Close engines
    if (main_engine_) {
        main_engine_->close();
    }
}

void BacktestEngine::close() {
    if (main_engine_) {
        main_engine_->close();
    }
}

BacktestRunSummary
run_backtest_multi(const std::vector<std::string>& parquet_files, const std::string& strategy_name,
                   double fee_rate, double slippage_bps, double risk_free_rate,
                   const std::string& iv_price_mode, int log_level,
                   const std::unordered_map<std::string, double>& strategy_setting) {
    BacktestRunSummary summary;
    if (parquet_files.empty() || strategy_name.empty()) {
        return summary;
    }

    std::vector<Metric>& metrics = summary.metrics;
    size_t estimated_total_metrics = parquet_files.size() * 390;
    metrics.reserve(estimated_total_metrics + estimated_total_metrics / 5);

    std::vector<DailyResult>& daily_results = summary.daily_results;
    daily_results.resize(parquet_files.size());
    std::vector<double>& daily_returns = summary.daily_returns;
    daily_returns.resize(parquet_files.size());

    auto overall_start_time = std::chrono::system_clock::now();
    std::chrono::system_clock::time_point overall_end_time;

    auto run_one_file = [&](BacktestEngine& file_engine, size_t file_idx,
                            std::vector<Metric>* out_metrics, int* base_timestep_val) {
        file_engine.reset();
        std::vector<Metric> file_metrics;
        file_metrics.reserve(400);
        file_engine.register_timestep_callback([&](int timestep, Timestamp ts) {
            Metric m;
            m.timestep = timestep;
            m.timestamp = ts_to_iso(ts);
            m.fees = file_engine.get_cumulative_fees();
            auto* me = file_engine.main_engine();
            if (me && me->option_strategy_engine()) {
                auto* holding = me->option_strategy_engine()->get_strategy_holding();
                if (holding) {
                    m.pnl = holding->summary.pnl;
                    m.delta = holding->summary.delta;
                    m.gamma = holding->summary.gamma;
                    m.theta = holding->summary.theta;
                }
            }
            file_metrics.push_back(m);
        });
        file_engine.load_backtest_data(parquet_files[file_idx]);
        file_engine.add_strategy(strategy_name, strategy_setting);
        if (auto* me = file_engine.main_engine()) {
            if (auto* de = me->get_data_engine()) {
                de->set_risk_free_rate(risk_free_rate);
                de->set_iv_price_mode(iv_price_mode);
            }
        }
        BacktestResult file_result = file_engine.run();

        DailyResult daily;
        daily.file_path = parquet_files[file_idx];
        daily.result = file_result;
        daily.daily_pnl = file_result.final_pnl;
        daily.daily_fees = file_engine.get_cumulative_fees();
        daily.file_index = file_idx;
        double daily_net_pnl = daily.daily_pnl - daily.daily_fees;

        daily_results[file_idx] = daily;
        daily_returns[file_idx] = daily_net_pnl;

        if (out_metrics && base_timestep_val) {
            for (auto& m : file_metrics) {
                m.timestep = *base_timestep_val + m.timestep;
                out_metrics->push_back(std::move(m));
            }
            if (!file_metrics.empty())
                *base_timestep_val += file_metrics.back().timestep + 1;
        }
    };

    if (parquet_files.size() == 1) {
        BacktestEngine engine;
        engine.configure_execution(fee_rate, slippage_bps);
        engine.main_engine()->set_log_level(log_level);
        int base = 0;
        run_one_file(engine, 0, &metrics, &base);
        overall_end_time = std::chrono::system_clock::now();
    } else {
        std::mutex results_mutex;
        std::atomic<int> completed_count{0};
        constexpr int num_engines = 4;
        std::queue<size_t> file_queue;
        for (size_t i = 0; i < parquet_files.size(); ++i)
            file_queue.push(i);

        std::stop_source stop_src;
        auto worker = [&](std::stop_token jthread_token, int engine_id) {
            (void)engine_id;
            std::stop_token shared_token = stop_src.get_token();
            while (!jthread_token.stop_requested() && !shared_token.stop_requested()) {
                size_t file_idx;
                {
                    std::lock_guard<std::mutex> lock(results_mutex);
                    if (file_queue.empty())
                        break;
                    file_idx = file_queue.front();
                    file_queue.pop();
                }
                if (jthread_token.stop_requested() || shared_token.stop_requested())
                    break;

                BacktestEngine file_engine;
                file_engine.configure_execution(fee_rate, slippage_bps);
                file_engine.main_engine()->set_log_level(log_level);

                std::vector<Metric> file_metrics;
                file_metrics.reserve(400);
                file_engine.reset();
                file_engine.register_timestep_callback([&](int timestep, Timestamp ts) {
                    Metric m;
                    m.timestep = timestep;
                    m.timestamp = ts_to_iso(ts);
                    m.fees = file_engine.get_cumulative_fees();
                    auto* me = file_engine.main_engine();
                    if (me && me->option_strategy_engine()) {
                        auto* holding = me->option_strategy_engine()->get_strategy_holding();
                        if (holding) {
                            m.pnl = holding->summary.pnl;
                            m.delta = holding->summary.delta;
                            m.gamma = holding->summary.gamma;
                            m.theta = holding->summary.theta;
                        }
                    }
                    file_metrics.push_back(m);
                });
                file_engine.load_backtest_data(parquet_files[file_idx]);
                file_engine.add_strategy(strategy_name, strategy_setting);
                if (auto* me = file_engine.main_engine()) {
                    if (auto* de = me->get_data_engine()) {
                        de->set_risk_free_rate(risk_free_rate);
                        de->set_iv_price_mode(iv_price_mode);
                    }
                }
                BacktestResult file_result = file_engine.run();

                DailyResult daily;
                daily.file_path = parquet_files[file_idx];
                daily.result = file_result;
                daily.daily_pnl = file_result.final_pnl;
                daily.daily_fees = file_engine.get_cumulative_fees();
                daily.file_index = file_idx;
                daily.file_metrics = std::move(file_metrics);
                double daily_net_pnl = daily.daily_pnl - daily.daily_fees;

                {
                    std::lock_guard<std::mutex> lock(results_mutex);
                    daily_results[file_idx] = std::move(daily);
                    daily_returns[file_idx] = daily_net_pnl;
                    completed_count.fetch_add(1);
                }
            }
        };

        {
            std::vector<std::jthread> threads;
            threads.reserve(num_engines);
            for (int i = 0; i < num_engines; ++i)
                threads.emplace_back(worker, i);
        }
        overall_end_time = std::chrono::system_clock::now();

        // Merge per-file metrics in file_index order
        size_t reserve_total = 0;
        for (const auto& daily : daily_results)
            reserve_total += daily.file_metrics.size();
        metrics.clear();
        metrics.reserve(reserve_total);
        int base = 0;
        for (size_t i = 0; i < daily_results.size(); ++i) {
            for (auto& m : daily_results[i].file_metrics) {
                m.timestep += base;
                metrics.push_back(std::move(m));
            }
            if (!daily_results[i].file_metrics.empty())
                base += static_cast<int>(daily_results[i].file_metrics.size());
        }
    }

    auto duration_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(overall_end_time - overall_start_time)
            .count();
    summary.duration_ms = duration_ms;
    summary.duration_seconds = duration_ms / 1000.0;

    // Aggregate results across all files
    BacktestResult aggregated_result;
    aggregated_result.strategy_name = strategy_name;
    aggregated_result.portfolio_name = "backtest";

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

} // namespace backtest
