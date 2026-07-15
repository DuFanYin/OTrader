#include "engine_gateway_backtest.hpp"
#include "../../runtime/backtest/engine_main.hpp"

namespace backtest {

BacktestGateway::BacktestGateway(MainEngine* main_engine, double fee_rate, double slippage_bps)
    : main_engine_(main_engine), fee_rate_(fee_rate), slippage_bps_(slippage_bps) {}

void BacktestGateway::configure_execution(double fee_rate, double slippage_bps) {
    fee_rate_ = fee_rate;
    slippage_bps_ = slippage_bps < 0.0 ? 0.0 : slippage_bps;
}

std::pair<double, double> BacktestGateway::get_market_bid_ask(const std::string& symbol) const {
    if (!main_engine_) {
        return {0.0, 0.0};
    }
    // Backtest uses single portfolio "backtest"; avoid depending on strategy types here.
    utilities::PortfolioData* portfolio = main_engine_->get_portfolio("backtest");
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

double BacktestGateway::default_contract_size(const std::string& symbol) {
    return symbol.ends_with(".STK") ? 1.0 : 100.0;
}

double BacktestGateway::calculate_order_fee(const utilities::OrderRequest& req,
                                            double fill_price) const {
    (void)fill_price;
    if (fee_rate_ <= 0.0 || !main_engine_) {
        return 0.0;
    }

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
    return total_contracts * fee_rate_;
}

bool BacktestGateway::execute_order(const utilities::OrderRequest& req, const std::string& orderid,
                                    int& trade_counter, double& cumulative_fees) {
    const double limit = req.price;
    const bool is_limit_order = (req.type == utilities::OrderType::LIMIT && limit > 0.0);

    double fill_price = 0.0;
    bool filled = false;

    if (is_limit_order) {
        // LIMIT: buy fill at ask iff limit>=ask; sell at bid iff limit<=bid
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
        // MARKET: buy at ask, sell at bid
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
        // Slippage (market only)
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

    if (main_engine_) {
        // Only emit an Order event when the status changes, to better match live gateway behavior
        // and avoid spamming NOTTRADED on every timestep for open orders.
        bool should_emit_order = true;
        if (!filled) {
            if (auto* existing = main_engine_->get_order(orderid); existing != nullptr) {
                if (existing->status == utilities::Status::NOTTRADED && existing->traded == 0.0 &&
                    existing->volume == order.volume) {
                    should_emit_order = false;
                }
            }
        }
        if (should_emit_order) {
            main_engine_->add_order(orderid, order);
            utilities::OrderData* order_slot = main_engine_->acquire_order();
            if (order_slot != nullptr) {
                *order_slot = order;
                main_engine_->put_event(utilities::Event(utilities::EventType::Order, order_slot));
            }
        }
    }

    if (filled && main_engine_) {
        trade_counter++;
        utilities::TradeData trade;
        trade.gateway_name = "Backtest";
        trade.symbol = req.symbol;
        trade.exchange = req.exchange;
        trade.tradeid = "backtest_trade_" + std::to_string(trade_counter);
        trade.orderid = orderid;
        trade.direction = req.direction;
        trade.price = fill_price;
        trade.volume = req.volume;
        trade.datetime = std::chrono::system_clock::now();
        utilities::TradeData* trade_slot = main_engine_->acquire_trade();
        if (trade_slot != nullptr) {
            *trade_slot = trade;
            main_engine_->put_event(utilities::Event(utilities::EventType::Trade, trade_slot));
        }

        if (req.is_combo && req.legs) {
            int i = 0;
            for (const auto& leg : *req.legs) {
                if (!leg.symbol) {
                    continue;
                }
                auto [leg_bid, leg_ask] = get_market_bid_ask(*leg.symbol);
                double leg_price =
                    (leg.direction == utilities::Direction::LONG) ? leg_ask : leg_bid;
                if (leg_price <= 0.0) {
                    leg_price = fill_price; // fallback for combo aggregate
                }
                utilities::TradeData leg_trade;
                leg_trade.gateway_name = "Backtest";
                leg_trade.symbol = *leg.symbol;
                leg_trade.exchange = leg.exchange;
                leg_trade.tradeid = "backtest_trade_" + std::to_string(trade_counter) + "_leg_" +
                                    std::to_string(i++);
                leg_trade.orderid = orderid;
                leg_trade.direction = leg.direction;
                leg_trade.price = leg_price;
                leg_trade.volume = req.volume * std::abs(static_cast<double>(leg.ratio));
                leg_trade.datetime = std::chrono::system_clock::now();
                utilities::TradeData* leg_slot = main_engine_->acquire_trade();
                if (leg_slot != nullptr) {
                    *leg_slot = leg_trade;
                    main_engine_->put_event(
                        utilities::Event(utilities::EventType::Trade, leg_slot));
                }
            }
        }

        const double fee = calculate_order_fee(req, fill_price);
        cumulative_fees += fee;
    }
    return filled;
}

} // namespace backtest
