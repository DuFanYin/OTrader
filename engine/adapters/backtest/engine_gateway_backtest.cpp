#include "engine_gateway_backtest.hpp"
#include "../../model/symbol.hpp"
#include <algorithm>

namespace adapters {

namespace {

constexpr double kShareFee = 0.005;      // USD a share (IBKR Pro, fixed)
constexpr double kShareFeeMinimum = 1.0; // USD an order

/** A combo's net price, as IB prices a BAG: bought legs minus sold legs (negative: a credit). */
auto combo_net_price(std::span<const model::Leg> legs, const std::vector<double>& leg_prices)
    -> double {
    double net = 0.0;
    for (size_t i = 0; i < leg_prices.size(); ++i) {
        const model::Leg& leg = legs[i];
        const double sign = (leg.direction == model::Direction::LONG) ? 1.0 : -1.0;
        net += sign * leg_prices[i] * std::abs(static_cast<double>(leg.ratio));
    }
    return net;
}

} // namespace

BacktestGateway::BacktestGateway(const model::PortfolioStructure& portfolios, double fee_rate,
                                 double slippage_bps, std::string id_prefix)
    : Gateway(nullptr, "Backtest"), portfolios_(portfolios), fee_rate_(fee_rate),
      slippage_bps_(slippage_bps), id_prefix_(std::move(id_prefix)) {}

auto BacktestGateway::portfolio_of(const model::OrderRequest& req) const
    -> const model::PortfolioData* {
    return portfolios_.portfolio_of(req.is_combo && !req.legs.empty() ? req.legs.front().symbol
                                                                      : req.symbol);
}

void BacktestGateway::report_order(const model::OrderData& order) {
    if (order.is_active()) {
        reported_[order.orderid] = order;
    } else {
        reported_.erase(order.orderid);
    }
    put_order(order);
}

auto BacktestGateway::send_order(const model::OrderRequest& req) -> std::string {
    ++order_counter_;
    std::string orderid = id_prefix_ + "_order_" + std::to_string(order_counter_);
    open_orders_.emplace(orderid, req);
    return orderid;
}

void BacktestGateway::cancel_order(const model::CancelRequest& req) {
    open_orders_.erase(req.orderid);
    // Reported cancelled when the engine has heard of it (it waited a bar, not filled).
    if (const auto it = reported_.find(req.orderid); it != reported_.end()) {
        model::OrderData cancelled = it->second;
        cancelled.status = model::Status::CANCELLED;
        report_order(cancelled);
    }
}

void BacktestGateway::execute_pending_orders() {
    std::vector<std::string> filled;
    filled.reserve(open_orders_.size());
    for (const auto& [orderid, req] : open_orders_) {
        if (execute_order(req, orderid)) {
            filled.push_back(orderid);
        }
    }
    for (const std::string& orderid : filled) {
        open_orders_.erase(orderid);
    }
}

void BacktestGateway::configure_execution(double fee_rate, double slippage_bps) {
    fee_rate_ = fee_rate;
    slippage_bps_ = slippage_bps < 0.0 ? 0.0 : slippage_bps;
}

std::pair<double, double> BacktestGateway::get_market_bid_ask(const std::string& symbol) const {
    const model::PortfolioData* portfolio = portfolios_.portfolio_of(symbol);
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
    return model::parse_option_symbol(symbol) ? 100.0 : 1.0;
}

double BacktestGateway::calculate_order_fee(const model::OrderRequest& req,
                                            double fill_price) const {
    (void)fill_price;
    if (fee_rate_ <= 0.0) {
        return 0.0;
    }

    if (!req.is_combo && !model::parse_option_symbol(req.symbol)) {
        // Shares: IBKR's fixed rate, a share's fee and an order's least, not an option's fee each
        return std::max(kShareFeeMinimum, kShareFee * std::abs(req.volume));
    }
    double total_contracts = 0.0;
    if (req.is_combo) {
        for (const auto& leg : req.legs) {
            if (leg.symbol.empty()) {
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

auto BacktestGateway::slipped(double price, model::Direction direction) const -> double {
    const double mult = 1.0 + (slippage_bps_ / 10000.0);
    return direction == model::Direction::LONG ? price * mult : price * (2.0 - mult);
}

auto BacktestGateway::combo_leg_prices(std::span<const model::Leg> legs, bool market) const
    -> std::optional<std::vector<double>> {
    std::vector<double> prices;
    prices.reserve(legs.size());
    for (const model::Leg& leg : legs) {
        if (leg.symbol.empty()) {
            return std::nullopt;
        }
        auto [bid, ask] = get_market_bid_ask(leg.symbol);
        // Each leg fills on its own side: a bought leg needs an ask; a sold leg takes the bid,
        // zero included (it sells for nothing).
        if ((bid <= 0 && ask <= 0) || (leg.direction == model::Direction::LONG && ask <= 0)) {
            return std::nullopt;
        }
        const double price = (leg.direction == model::Direction::LONG) ? ask : bid;
        prices.push_back(market ? slipped(price, leg.direction) : price);
    }
    return prices;
}

bool BacktestGateway::execute_order(const model::OrderRequest& req,
                                    const std::string& orderid) {
    const double limit = req.price;

    double fill_price = 0.0;
    bool filled = false;
    std::vector<double> leg_prices; // a combo's fill price per leg, in req.legs order

    if (req.is_combo && !req.legs.empty()) {
        // A combo trades the way the IB gateway sends it (a BAG bought with each leg's own side):
        // every leg fills on its side, the combo at the net price (bought legs minus sold legs;
        // negative is a credit), and a limit caps that net price, whatever its sign.
        const bool is_limit = req.type == model::OrderType::LIMIT;
        if (auto prices = combo_leg_prices(req.legs, /*market=*/!is_limit)) {
            const double net = combo_net_price(req.legs, *prices);
            if (!is_limit || net <= limit) {
                fill_price = net;
                filled = true;
                leg_prices = std::move(*prices);
            }
        }
    } else if (req.type == model::OrderType::LIMIT && limit > 0.0) {
        // LIMIT: buy fill at ask iff limit>=ask; sell at bid iff limit<=bid
        auto [bid, ask] = get_market_bid_ask(req.symbol);
        if (req.direction == model::Direction::LONG) {
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
    } else {
        // MARKET: buy at ask, sell at bid, with slippage
        auto [bid, ask] = get_market_bid_ask(req.symbol);
        if (req.direction == model::Direction::LONG) {
            fill_price = ask;
            filled = ask > 0.0;
        } else {
            fill_price = bid;
            filled = bid > 0.0;
        }
        if (filled) {
            fill_price = slipped(fill_price, req.direction);
        }
    }

    model::OrderData order = req.create_order_data(orderid);
    if (filled) {
        order.status = model::Status::ALLTRADED;
        order.traded = order.volume;
    } else {
        order.status = model::Status::NOTTRADED;
        order.traded = 0;
    }

    // Only emit an Order event when the status changes, to better match live gateway behavior
    // and avoid spamming NOTTRADED on every timestep for open orders.
    bool should_emit_order = true;
    if (!filled) {
        if (const auto it = reported_.find(orderid); it != reported_.end()) {
            const model::OrderData& existing = it->second;
            if (existing.status == model::Status::NOTTRADED && existing.traded == 0.0 &&
                existing.volume == order.volume) {
                should_emit_order = false;
            }
        }
    }
    if (should_emit_order) {
        report_order(order);
    }

    if (filled) {
        // Fills are stamped with the event time: the bar's, not the machine's clock.
        const model::PortfolioData* portfolio = portfolio_of(req);
        const model::DateTime at =
            portfolio != nullptr ? portfolio->datetime() : model::DateTime{};
        ++trade_counter_;
        // A fill is a contract's: a combo fills as its legs, nothing of its own.
        if (leg_prices.empty()) {
            model::TradeData trade;
            trade.symbol = req.symbol;
            trade.tradeid = id_prefix_ + "_trade_" + std::to_string(trade_counter_);
            trade.orderid = orderid;
            trade.direction = req.direction;
            trade.price = fill_price;
            trade.volume = req.volume;
            trade.datetime = at;
            put_trade(trade);
        }

        for (size_t i = 0; i < leg_prices.size(); ++i) {
            const auto& leg = req.legs[i];
            model::TradeData leg_trade;
            leg_trade.symbol = leg.symbol;
            leg_trade.tradeid =
                id_prefix_ + "_trade_" + std::to_string(trade_counter_) + "_leg_" + std::to_string(i);
            leg_trade.orderid = orderid;
            leg_trade.direction = leg.direction;
            leg_trade.price = leg_prices[i];
            leg_trade.volume = req.volume * std::abs(static_cast<double>(leg.ratio));
            leg_trade.datetime = at;
            put_trade(leg_trade);
        }

        const double fee = calculate_order_fee(req, fill_price);
        cumulative_fees_ += fee;
    }
    return filled;
}

} // namespace adapters
