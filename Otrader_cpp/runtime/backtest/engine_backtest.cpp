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
#include <stdexcept>
#include <utility>

namespace backtest {

BacktestEngine::BacktestEngine() {
    event_engine_ = std::make_unique<EventEngine>();
    main_engine_ = std::make_unique<MainEngine>(event_engine_.get());
    event_engine_->set_main_engine(main_engine_.get());
    // Orders are queued and executed at next timestep (not same step)
    main_engine_->set_order_executor([this](const utilities::OrderRequest& req) -> std::string {
        return this->submit_order(req);
    });
}

void BacktestEngine::configure_execution(double fee_rate, double slippage_bps) {
    if (fee_rate < 0.0) {
        throw std::runtime_error("fee_rate must be >= 0");
    }
    if (slippage_bps < 0.0) {
        slippage_bps = 0.0;
    }
    fee_rate_ = fee_rate;
    slippage_bps_ = slippage_bps;
}

auto BacktestEngine::get_market_bid_ask(const std::string& symbol) const
    -> std::pair<double, double> {
    if (!main_engine_ || (main_engine_->option_strategy_engine() == nullptr)) {
        return {0.0, 0.0};
    }
    auto* strategy = main_engine_->option_strategy_engine()->get_strategy();
    if (strategy == nullptr) {
        return {0.0, 0.0};
    }
    utilities::PortfolioData* portfolio = main_engine_->get_portfolio(strategy->portfolio_name());
    if (portfolio == nullptr) {
        return {0.0, 0.0};
    }
    auto it = portfolio->options.find(symbol);
    if (it != portfolio->options.end()) {
        const auto& opt = it->second;
        return {opt.bid_price, opt.ask_price};
    }
    if (portfolio->underlying && portfolio->underlying->symbol == symbol) {
        const auto& und = *portfolio->underlying;
        return {und.bid_price, und.ask_price};
    }
    return {0.0, 0.0};
}

auto BacktestEngine::default_contract_size(const std::string& symbol) -> double {
    return symbol.ends_with(".STK") ? 1.0 : 100.0;
}

auto BacktestEngine::calculate_order_fee(const utilities::OrderRequest& req,
                                         double fill_price) const -> double {
    if (fee_rate_ <= 0.0 || !main_engine_) {
        return 0.0;
    }

    // fee_rate is per contract fee (e.g., $0.35 per contract)
    // Calculate total contracts traded
    double total_contracts = 0.0;
    if (req.is_combo && req.legs) {
        for (const auto& leg : *req.legs) {
            if (!leg.symbol) {
                continue;
            }
            const double leg_volume =
                std::abs(req.volume * std::abs(static_cast<double>(leg.ratio)));
            total_contracts += leg_volume;
        }
    } else {
        total_contracts = std::abs(req.volume);
    }

    // Fee = number of contracts * fee per contract
    return total_contracts * fee_rate_;
}

std::string BacktestEngine::submit_order(const utilities::OrderRequest& req) {
    order_counter_++;
    std::string orderid = "backtest_order_" + std::to_string(order_counter_);
    pending_orders_.emplace_back(orderid, req);
    return orderid;
}

void BacktestEngine::execute_order_impl(const utilities::OrderRequest& req,
                                        const std::string& orderid) {
    const double limit = req.price;
    const bool is_limit_order =
        (req.type == utilities::OrderType::LIMIT && limit > 0.0);

    double fill_price = 0.0;
    bool filled = false;

    if (is_limit_order) {
        // Crossing Model (strict): no mid fantasy; fill only when limit crosses.
        // Buy: fill at ask iff limit >= ask. Sell: fill at bid iff limit <= bid.
        if (req.is_combo && req.legs && !req.legs->empty()) {
            double total_bid = 0.0;
            double total_ask = 0.0;
            bool ok = true;
            for (const auto& leg : *req.legs) {
                if (!leg.symbol) {
                    ok = false;
                    break;
                }
                auto [bid, ask] = get_market_bid_ask(*leg.symbol);
                if (bid <= 0 && ask <= 0) {
                    ok = false;
                    break;
                }
                const double q = std::abs(static_cast<double>(leg.ratio));
                total_bid += bid * q;
                total_ask += ask * q;
            }
            if (ok) {
                if (req.direction == utilities::Direction::LONG) {
                    if (limit >= total_ask && total_ask > 0.0) {
                        fill_price = total_ask;
                        filled = true;
                    }
                } else {
                    if (limit <= total_bid && total_bid > 0.0) {
                        fill_price = total_bid;
                        filled = true;
                    }
                }
            }
        } else {
            auto [bid, ask] = get_market_bid_ask(req.symbol);
            if (req.direction == utilities::Direction::LONG) {
                if (limit >= ask && ask > 0.0) {
                    fill_price = ask;
                    filled = true;
                }
            } else {
                if (limit <= bid && bid > 0.0) {
                    fill_price = bid;
                    filled = true;
                }
            }
        }
    } else {
        // MARKET: buy at ask, sell at bid (no mid)
        if (req.is_combo && req.legs && !req.legs->empty()) {
            double total_bid = 0.0;
            double total_ask = 0.0;
            bool ok = true;
            for (const auto& leg : *req.legs) {
                if (!leg.symbol) {
                    ok = false;
                    break;
                }
                auto [bid, ask] = get_market_bid_ask(*leg.symbol);
                if (bid <= 0 && ask <= 0) {
                    ok = false;
                    break;
                }
                const double q = std::abs(static_cast<double>(leg.ratio));
                total_bid += bid * q;
                total_ask += ask * q;
            }
            if (ok) {
                if (req.direction == utilities::Direction::LONG) {
                    fill_price = total_ask;
                    filled = total_ask > 0.0;
                } else {
                    fill_price = total_bid;
                    filled = total_bid > 0.0;
                }
            }
        } else {
            auto [bid, ask] = get_market_bid_ask(req.symbol);
            if (req.direction == utilities::Direction::LONG) {
                fill_price = ask;
                filled = ask > 0.0;
            } else {
                fill_price = bid;
                filled = bid > 0.0;
            }
        }
        // Slippage only for market path (worse fill: buy higher, sell lower)
        if (filled && slippage_bps_ > 0.0 && fill_price > 0.0) {
            const double mult = 1.0 + (slippage_bps_ / 10000.0);
            if (req.direction == utilities::Direction::LONG) {
                fill_price *= mult;
            } else {
                fill_price *= (2.0 - mult);
            }
        }
    }

    utilities::OrderData order = req.create_order_data(orderid, "Backtest");
    if (filled) {
        order.status = utilities::Status::ALLTRADED;
        order.traded = order.volume;
    } else {
        order.status = utilities::Status::NOTTRADED;
        order.traded = 0;
    }

    main_engine_->add_order(orderid, order);
    main_engine_->put_event(utilities::Event(utilities::EventType::Order, order));

    if (filled) {
        trade_counter_++;
        utilities::TradeData trade;
        trade.gateway_name = "Backtest";
        trade.symbol = req.symbol;
        trade.exchange = req.exchange;
        trade.tradeid = "backtest_trade_" + std::to_string(trade_counter_);
        trade.orderid = orderid;
        trade.direction = req.direction;
        trade.price = fill_price;
        trade.volume = req.volume;
        trade.datetime = std::chrono::system_clock::now();
        main_engine_->put_event(utilities::Event(utilities::EventType::Trade, trade));

        if (req.is_combo && req.legs) {
            int i = 0;
            for (const auto& leg : *req.legs) {
                if (!leg.symbol) {
                    continue;
                }
                auto [leg_bid, leg_ask] = get_market_bid_ask(*leg.symbol);
                double leg_price = (leg.direction == utilities::Direction::LONG) ? leg_ask : leg_bid;
                if (leg_price <= 0.0) {
                    leg_price = fill_price; // fallback for combo aggregate
                }
                utilities::TradeData leg_trade;
                leg_trade.gateway_name = "Backtest";
                leg_trade.symbol = *leg.symbol;
                leg_trade.exchange = leg.exchange;
                leg_trade.tradeid =
                    "backtest_trade_" + std::to_string(trade_counter_) + "_leg_" + std::to_string(i++);
                leg_trade.orderid = orderid;
                leg_trade.direction = leg.direction;
                leg_trade.price = leg_price;
                leg_trade.volume = req.volume * std::abs(static_cast<double>(leg.ratio));
                leg_trade.datetime = std::chrono::system_clock::now();
                main_engine_->put_event(utilities::Event(utilities::EventType::Trade, leg_trade));
            }
        }
        const double fee = calculate_order_fee(req, fill_price);
        if (fee > 0.0) {
            cumulative_fees_ += fee;
        }
    }
}

void BacktestEngine::execute_pending_orders() {
    for (auto& [orderid, req] : pending_orders_) {
        execute_order_impl(req, orderid);
    }
    pending_orders_.clear();
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
    // 固定使用名为 "backtest" 的组合，由 MainEngine/DataEngine 负责注册
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
            event_engine_->put_event(utilities::Event(
                utilities::EventType::Snapshot, data_engine->get_precomputed_snapshot(step_count)));
            current_timestep_ = step_count + 1;
            total_rows += frame.num_rows;

            // Next-bar execution: orders sent at Timer(t) only fill after Snapshot(t+1) is applied.
            // So we execute pending (from Timer(step_count-1)) now that we "see" this bar's BBO.
            execute_pending_orders();

            // Timer(step_count): strategy runs with this bar's info and may send orders (pending for next step).
            event_engine_->put_event(utilities::Event(utilities::EventType::Timer));

            auto* holding = strategy_engine->get_strategy_holding();
            if (holding) {
                current_pnl_ = holding->summary.pnl;
                current_delta_ = holding->summary.delta;
                max_delta_ = std::max(std::abs(holding->summary.delta), max_delta_);
                max_gamma_ = std::max(std::abs(holding->summary.gamma), max_gamma_);
                max_theta_ = std::max(std::abs(holding->summary.theta), max_theta_);

                // Track peak PnL and calculate drawdown
                // Initialize peak_pnl_ with first PnL value if not set yet
                if (step_count == 0) {
                    peak_pnl_ = current_pnl_;
                } else {
                    // Update peak if we've reached a new high
                    peak_pnl_ = std::max(current_pnl_, peak_pnl_);
                }
                // Calculate drawdown from peak
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
    pending_orders_.clear();
    order_counter_ = 0;
    trade_counter_ = 0;

    // Clear strategy (but keep engine alive)
    if (main_engine_ && (main_engine_->option_strategy_engine() != nullptr)) {
        auto* strategy_engine = main_engine_->option_strategy_engine();
        // Stop and clear strategy if exists
        if (strategy_engine->get_strategy() != nullptr) {
            strategy_engine->get_strategy()->on_stop();
        }
    }
}

void BacktestEngine::close() {
    if (main_engine_) {
        main_engine_->close();
    }
}

} // namespace backtest
