#include "engine_backtest.hpp"
#include "engine_main.hpp"
#include "../../core/engine_option_strategy.hpp"
#include "../../strategy/template.hpp"
#include "../../utilities/event.hpp"
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <stdexcept>

namespace backtest {

BacktestEngine::BacktestEngine() {
    event_engine_ = std::make_unique<EventEngine>();
    main_engine_ = std::make_unique<MainEngine>(event_engine_.get());
    event_engine_->set_main_engine(main_engine_.get());
    main_engine_->set_order_executor([this](const utilities::OrderRequest& req) {
        return this->execute_order(req);
    });
}

void BacktestEngine::configure_execution(const std::string& fill_mode, double fee_rate) {
    std::string mode = fill_mode;
    if (mode.empty()) mode = "mid";
    if (mode != "mid" && mode != "bid" && mode != "ask") {
        throw std::runtime_error("Unsupported fill_mode: " + mode);
    }
    if (fee_rate < 0.0) {
        throw std::runtime_error("fee_rate must be >= 0");
    }
    fill_mode_ = mode;
    fee_rate_ = fee_rate;
}

double BacktestEngine::select_fill_price(double bid, double ask, double mid, utilities::Direction direction) const {
    (void)direction;
    if (fill_mode_ == "bid") {
        if (bid > 0) return bid;
        return mid > 0 ? mid : ask;
    }
    if (fill_mode_ == "ask") {
        if (ask > 0) return ask;
        return mid > 0 ? mid : bid;
    }
    if (mid > 0) return mid;
    if (bid > 0 && ask > 0) return (bid + ask) / 2.0;
    return ask > 0 ? ask : bid;
}

double BacktestEngine::get_market_price(const std::string& symbol, utilities::Direction direction) const {
    if (!main_engine_ || !main_engine_->option_strategy_engine()) return 0.0;
    auto* strategy = main_engine_->option_strategy_engine()->get_strategy();
    if (!strategy) return 0.0;
    utilities::PortfolioData* portfolio = main_engine_->get_portfolio(strategy->portfolio_name());
    if (!portfolio) return 0.0;
    auto it = portfolio->options.find(symbol);
    if (it != portfolio->options.end()) {
        const auto& opt = it->second;
        return select_fill_price(opt.bid_price, opt.ask_price, opt.mid_price, direction);
    }
    if (portfolio->underlying && portfolio->underlying->symbol == symbol) {
        const auto& und = *portfolio->underlying;
        return select_fill_price(und.bid_price, und.ask_price, und.mid_price, direction);
    }
    return 0.0;
}

double BacktestEngine::default_contract_size(const std::string& symbol) const {
    return symbol.ends_with(".STK") ? 1.0 : 100.0;
}

double BacktestEngine::calculate_order_fee(const utilities::OrderRequest& req, double fill_price) const {
    if (fee_rate_ <= 0.0 || !main_engine_) return 0.0;
    
    // fee_rate is per contract fee (e.g., $0.35 per contract)
    // Calculate total contracts traded
    double total_contracts = 0.0;
    if (req.is_combo && req.legs) {
        for (const auto& leg : *req.legs) {
            if (!leg.symbol) continue;
            const double leg_volume = std::abs(req.volume * std::abs(static_cast<double>(leg.ratio)));
            total_contracts += leg_volume;
        }
    } else {
        total_contracts = std::abs(req.volume);
    }
    
    // Fee = number of contracts * fee per contract
    return total_contracts * fee_rate_;
}

std::string BacktestEngine::execute_order(const utilities::OrderRequest& req) {
    static int counter = 0;
    counter++;
    std::string orderid = "backtest_order_" + std::to_string(counter);

    double fill_price = req.price;
    if (fill_price <= 0 || req.type == utilities::OrderType::MARKET) {
        if (req.is_combo && req.legs && !req.legs->empty()) {
            double total = 0.0;
            bool ok = true;
            for (const auto& leg : *req.legs) {
                if (!leg.symbol) {
                    ok = false;
                    break;
                }
                double leg_price = get_market_price(*leg.symbol, leg.direction);
                if (leg_price <= 0) {
                    ok = false;
                    break;
                }
                total += leg_price * std::abs(static_cast<double>(leg.ratio));
            }
            if (ok) fill_price = total;
        } else {
            fill_price = get_market_price(req.symbol, req.direction);
        }
        if (fill_price <= 0) fill_price = req.price;
    }

    utilities::OrderData order = req.create_order_data(orderid, "Backtest");
    order.status = utilities::Status::ALLTRADED;
    order.traded = order.volume;

    main_engine_->add_order(orderid, order);
    main_engine_->put_event(utilities::Event(utilities::EventType::Order, order));
    utilities::TradeData trade;
    trade.gateway_name = "Backtest";
    trade.symbol = req.symbol;
    trade.exchange = req.exchange;
    trade.tradeid = "backtest_trade_" + std::to_string(counter);
    trade.orderid = orderid;
    trade.direction = req.direction;
    trade.price = fill_price;
    trade.volume = req.volume;
    trade.datetime = std::chrono::system_clock::now();
    main_engine_->put_event(utilities::Event(utilities::EventType::Trade, trade));

    if (req.is_combo && req.legs) {
        int i = 0;
        for (const auto& leg : *req.legs) {
            if (!leg.symbol) continue;
            utilities::TradeData leg_trade;
            leg_trade.gateway_name = "Backtest";
            leg_trade.symbol = *leg.symbol;
            leg_trade.exchange = leg.exchange;
            leg_trade.tradeid = "backtest_trade_" + std::to_string(counter) + "_leg_" + std::to_string(i++);
            leg_trade.orderid = orderid;
            leg_trade.direction = leg.direction;
            double leg_price = get_market_price(*leg.symbol, leg.direction);
            leg_trade.price = leg_price > 0 ? leg_price : fill_price;
            leg_trade.volume = req.volume * std::abs(static_cast<double>(leg.ratio));
            leg_trade.datetime = std::chrono::system_clock::now();
            main_engine_->put_event(utilities::Event(utilities::EventType::Trade, leg_trade));
        }
    }
    const double fee = calculate_order_fee(req, fill_price);
    if (fee > 0.0) cumulative_fees_ += fee;
    return orderid;
}

void BacktestEngine::load_backtest_data(std::string const& parquet_path,
                                        std::string const& underlying_symbol) {
    if (main_engine_)
        main_engine_->load_backtest_data(parquet_path, underlying_symbol);
}

void BacktestEngine::add_strategy(std::string const& strategy_name,
                                  std::unordered_map<std::string, double> const& setting) {
    strategy_name_ = strategy_name;
    strategy_setting_ = setting;
    // 固定使用名为 "backtest" 的组合，由 MainEngine/DataEngine 负责注册
    std::string portfolio_name = "backtest";
    if (main_engine_ && main_engine_->option_strategy_engine())
        main_engine_->option_strategy_engine()->add_strategy(strategy_name, portfolio_name, setting);
}

void BacktestEngine::register_timestep_callback(TimestepCallback cb) {
    timestep_callbacks_.push_back(std::move(cb));
}

std::unordered_map<std::string, double> BacktestEngine::get_current_state() const {
    std::unordered_map<std::string, double> m;
    m["pnl"] = current_pnl_;
    m["delta"] = current_delta_;
    if (main_engine_ && main_engine_->option_strategy_engine()) {
        auto* holding = main_engine_->option_strategy_engine()->get_strategy_holding();
        if (holding) {
            m["pnl"] = holding->summary.pnl;
            m["delta"] = holding->summary.delta;
        }
    }
    return m;
}

BacktestResult BacktestEngine::run() {
    BacktestResult result;
    result.strategy_name = strategy_name_;
    result.portfolio_name = "backtest";
    result.errors = errors_;

    BacktestDataEngine* data_engine = main_engine_ ? main_engine_->get_data_engine() : nullptr;
    if (!data_engine || !data_engine->has_data()) {
        result.errors.push_back("No data loaded. Call main_engine.load_backtest_data() first.");
        return result;
    }
    core::OptionStrategyEngine* strategy_engine = main_engine_ ? main_engine_->option_strategy_engine() : nullptr;
    if (!strategy_engine || !strategy_engine->get_strategy()) {
        result.errors.push_back("No strategy added. Call add_strategy() first.");
        return result;
    }
    auto* strategy = strategy_engine->get_strategy();
    if (strategy && !strategy->inited()) {
        strategy->on_init();
        strategy->on_start();
    }
    if (strategy) result.portfolio_name = strategy->portfolio_name();

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

    data_engine->iter_timesteps([this, &result, &start_time, &end_time, &step_count, &total_rows, data_engine, strategy_engine](
                                    Timestamp ts, TimestepFrameColumnar const& frame) {
        if (step_count == 0) start_time = ts;
        end_time = ts;
        event_engine_->put_event(utilities::Event(utilities::EventType::Snapshot,
                                                  data_engine->get_precomputed_snapshot(step_count)));
        current_timestep_ = step_count + 1;
        total_rows += frame.num_rows;

        event_engine_->put_event(utilities::Event(utilities::EventType::Timer));

        auto* holding = strategy_engine->get_strategy_holding();
        if (holding) {
            current_pnl_ = holding->summary.pnl;
            current_delta_ = holding->summary.delta;
            if (std::abs(holding->summary.delta) > max_delta_) max_delta_ = std::abs(holding->summary.delta);
            if (std::abs(holding->summary.gamma) > max_gamma_) max_gamma_ = std::abs(holding->summary.gamma);
            if (std::abs(holding->summary.theta) > max_theta_) max_theta_ = std::abs(holding->summary.theta);
            
            // Track peak PnL and calculate drawdown
            // Initialize peak_pnl_ with first PnL value if not set yet
            if (step_count == 0) {
                peak_pnl_ = current_pnl_;
            } else {
                // Update peak if we've reached a new high
                if (current_pnl_ > peak_pnl_) {
                    peak_pnl_ = current_pnl_;
                }
            }
            // Calculate drawdown from peak
            double drawdown = peak_pnl_ - current_pnl_;
            if (drawdown > max_drawdown_) {
                max_drawdown_ = drawdown;
            }
        }

        for (auto const& cb : timestep_callbacks_)
            cb(current_timestep_, ts);
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
    result.final_pnl = holding ? holding->summary.pnl : 0.0;
    result.max_delta = max_delta_;
    result.max_gamma = max_gamma_;
    result.max_theta = max_theta_;
    result.max_drawdown = max_drawdown_;
    result.total_orders = static_cast<int>(strategy_engine->get_all_orders().size());
    result.errors = errors_;
    if (strategy_engine->get_strategy() && !strategy_engine->get_strategy()->error_msg().empty())
        result.errors.push_back(strategy_engine->get_strategy()->error_msg());

    return result;
}

void BacktestEngine::reset() {
    // Reset internal state
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
    
    // Clear strategy (but keep engine alive)
    if (main_engine_ && main_engine_->option_strategy_engine()) {
        auto* strategy_engine = main_engine_->option_strategy_engine();
        // Stop and clear strategy if exists
        if (strategy_engine->get_strategy()) {
            strategy_engine->get_strategy()->on_stop();
        }
    }

}

void BacktestEngine::close() {
    if (main_engine_)
        main_engine_->close();
}

}  // namespace backtest
