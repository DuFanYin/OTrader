#include "adapters/gateway/backtest_gateway.hpp"
#include "model/symbol.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace adapters {

namespace {

constexpr double kShareFee = 0.005;      // USD a share (IBKR Pro, fixed)
constexpr double kShareFeeMinimum = 1.0; // USD an order

/** The net a unit, as IB prices a BAG: bought legs minus sold legs (negative: a credit). */
auto net_price(std::span<const model::Leg> legs, const std::vector<double>& leg_prices)
    -> double {
    double net = 0.0;
    for (size_t i = 0; i < leg_prices.size(); ++i) {
        const model::Leg& leg = legs[i];
        const double sign = (leg.side == model::Side::BUY) ? 1.0 : -1.0;
        net += sign * leg_prices[i] * std::abs(static_cast<double>(leg.ratio));
    }
    return net;
}

} // namespace

BacktestGateway::BacktestGateway(const model::Portfolios& portfolios, Execution execution,
                                 std::string id_prefix)
    : core::Gateway(nullptr, "Backtest"), portfolios_(portfolios),
      id_prefix_(std::move(id_prefix)) {
    configure_execution(execution);
}

auto BacktestGateway::portfolio_of(const model::OrderRequest& req) const
    -> const model::Portfolio* {
    return req.legs.empty() ? nullptr : portfolios_.portfolio_of(req.legs.front().symbol);
}

void BacktestGateway::report_order(const model::Order& order) {
    if (order.is_active()) {
        reported_[order.order_id] = order;
    } else {
        reported_.erase(order.order_id);
    }
    put_order(order);
}

auto BacktestGateway::send_order(const model::OrderRequest& req) -> std::string {
    ++order_counter_;
    std::string order_id = id_prefix_ + "_order_" + std::to_string(order_counter_);
    open_orders_.push_back(
        {.order_id = order_id, .req = req, .filled = 0.0, .wait = execution_.delay_bars});
    return order_id;
}

void BacktestGateway::cancel_order(const model::CancelRequest& req) {
    // As a venue does: an order it still has is reported cancelled; one done with, nothing.
    const auto pending = std::ranges::find(open_orders_, req.order_id, &Pending::order_id);
    if (pending == open_orders_.end()) {
        return;
    }
    model::Order cancelled = pending->req.create_order(pending->order_id);
    cancelled.filled = pending->filled;
    cancelled.status = model::Status::CANCELLED;
    open_orders_.erase(pending);
    report_order(cancelled);
}

void BacktestGateway::execute_pending_orders() {
    taken_at_ask_.clear();
    taken_at_bid_.clear();
    std::erase_if(open_orders_, [this](Pending& p) {
        if (p.wait > 0) {
            --p.wait;
            return false;
        }
        return execute_order(p);
    });
}

void BacktestGateway::configure_execution(Execution execution) {
    if (execution.fee_rate < 0.0) {
        throw std::invalid_argument("fee_rate must be >= 0");
    }
    if (execution.delay_bars < 0) {
        throw std::invalid_argument("delay_bars must be >= 0");
    }
    if (execution.fill_position < 0.0 || execution.multi_leg_fill_position < 0.0) {
        throw std::invalid_argument("a fill position must be >= 0");
    }
    execution_ = execution;
}

auto BacktestGateway::shown_left(const std::string& symbol, model::Side side) const
    -> double {
    const model::Portfolio* portfolio = portfolios_.portfolio_of(symbol);
    if (portfolio == nullptr) {
        return std::numeric_limits<double>::infinity();
    }
    const std::optional<model::Option> option = portfolio->option(symbol);
    if (!option) {
        return std::numeric_limits<double>::infinity();
    }
    const bool buying = side == model::Side::BUY;
    const auto& taken = buying ? taken_at_ask_ : taken_at_bid_;
    const auto t = taken.find(symbol);
    const double shown = buying ? option->ask_size() : option->bid_size();
    return std::max(shown - (t != taken.end() ? t->second : 0.0), 0.0);
}

auto BacktestGateway::fillable(const model::OrderRequest& req, double remaining) const -> double {
    if (execution_.fill == FillModel::Quote) {
        return remaining;
    }
    double left = remaining;
    for (const model::Leg& leg : req.legs) {
        const double ratio = std::abs(static_cast<double>(leg.ratio));
        if (ratio > 0.0) {
            left = std::min(left, shown_left(leg.symbol, leg.side) / ratio);
        }
    }
    return std::floor(left);
}

void BacktestGateway::take(const model::OrderRequest& req, double quantity) {
    for (const model::Leg& leg : req.legs) {
        (leg.side == model::Side::BUY ? taken_at_ask_ : taken_at_bid_)[leg.symbol] +=
            quantity * std::abs(static_cast<double>(leg.ratio));
    }
}

auto BacktestGateway::get_market_bid_ask(const std::string& symbol) const
    -> std::pair<double, double> {
    const model::Portfolio* portfolio = portfolios_.portfolio_of(symbol);
    if (portfolio == nullptr) {
        return {0.0, 0.0};
    }
    if (const std::optional<model::Option> option = portfolio->option(symbol)) {
        return {option->bid(), option->ask()};
    }
    if (const std::optional<model::Underlying> shares = portfolio->instrument(symbol)) {
        return {shares->bid(), shares->ask()};
    }
    return {0.0, 0.0};
}

auto BacktestGateway::default_contract_size(const std::string& symbol) -> double {
    return model::parse_option_symbol(symbol) ? 100.0 : 1.0;
}

auto BacktestGateway::calculate_order_fee(const model::OrderRequest& req, double quantity) const
    -> double {
    if (execution_.fee_rate <= 0.0) {
        return 0.0;
    }
    double contracts = 0.0;
    double shares = 0.0;
    for (const model::Leg& leg : req.legs) {
        const double units = std::abs(quantity * static_cast<double>(leg.ratio));
        (model::parse_option_symbol(leg.symbol) ? contracts : shares) += units;
    }
    // Options a contract each; shares IBKR's fixed rate, a share's fee and an order's least.
    return contracts * execution_.fee_rate +
           (shares > 0.0 ? std::max(kShareFeeMinimum, kShareFee * shares) : 0.0);
}

auto BacktestGateway::leg_prices(std::span<const model::Leg> legs) const
    -> std::optional<std::vector<double>> {
    const double position = legs.size() > 1 ? execution_.multi_leg_fill_position
                                            : execution_.fill_position;
    std::vector<double> prices;
    prices.reserve(legs.size());
    for (const model::Leg& leg : legs) {
        const auto [bid, ask] = get_market_bid_ask(leg.symbol);
        const bool buying = leg.side == model::Side::BUY;
        // Each leg fills on its own side. A bought leg needs an ask; a sold one in a combo may
        // sell for nothing (the other legs pay), but a contract sold alone needs a bid. A crossed
        // quote is none.
        if (leg.symbol.empty() || (bid <= 0 && ask <= 0) || (buying && ask <= 0) ||
            (legs.size() == 1 && !buying && bid <= 0) || (bid > 0 && ask > 0 && bid > ask)) {
            return std::nullopt;
        }
        const double low = std::max(bid, 0.0);
        const double high = ask > 0 ? ask : low;
        const double mid = (low + high) / 2.0;
        const double half = (high - low) / 2.0;
        prices.push_back(buying ? mid + position * half : std::max(mid - position * half, 0.0));
    }
    return prices;
}

auto BacktestGateway::execute_order(Pending& pending) -> bool {
    const model::OrderRequest& req = pending.req;
    const std::string& order_id = pending.order_id;

    // As IB fills a BAG: each leg on its own side, a limit capping the net a unit (paid
    // positive, received negative). A contract alone is the one-leg case of it.
    const bool is_limit = req.type == model::OrderType::LIMIT;
    std::optional<std::vector<double>> prices = leg_prices(req.legs);
    const bool priced =
        prices.has_value() && (!is_limit || net_price(req.legs, *prices) <= req.price);

    const double remaining = req.quantity - pending.filled;
    const double quantity = priced ? std::min(fillable(req, remaining), remaining) : 0.0;
    const bool filled = quantity > 0.0;
    if (filled) {
        pending.filled += quantity;
        take(req, quantity);
    }
    const bool whole = pending.filled >= req.quantity;

    model::Order order = req.create_order(order_id);
    order.filled = pending.filled;
    order.status = whole            ? model::Status::FILLED
                   : order.filled > 0 ? model::Status::PARTLY_FILLED
                                      : model::Status::WORKING;

    // An order's state goes to the engine when it changes, as a live venue's does: not again on
    // every bar it waits.
    const auto reported = reported_.find(order_id);
    const bool should_emit_order =
        filled || reported == reported_.end() || reported->second.status != order.status ||
        reported->second.filled != order.filled || reported->second.quantity != order.quantity;
    if (should_emit_order) {
        report_order(order);
    }

    if (filled) {
        // Fills are stamped with the event time: the bar's, not the machine's clock.
        const model::Portfolio* portfolio = portfolio_of(req);
        const model::DateTime at =
            portfolio != nullptr ? portfolio->datetime() : model::DateTime{};
        ++trade_counter_;
        const std::string fill_id = id_prefix_ + "_trade_" + std::to_string(trade_counter_);
        const auto fill = [&](const std::string& symbol, std::string id,
                              model::Side side, double price, double v) {
            model::Fill fill;
            fill.symbol = symbol;
            fill.fill_id = std::move(id);
            fill.order_id = order_id;
            fill.side = side;
            fill.price = price;
            fill.quantity = v;
            fill.datetime = at;
            return fill;
        };
        // A fill is a contract's: each leg's, a contract alone under the fill's own id.
        for (size_t i = 0; i < req.legs.size(); ++i) {
            const model::Leg& leg = req.legs[i];
            put_fill(fill(leg.symbol,
                          req.legs.size() == 1 ? fill_id : fill_id + "_leg_" + std::to_string(i),
                          leg.side, (*prices)[i],
                          quantity * std::abs(static_cast<double>(leg.ratio))));
        }

        cumulative_fees_ += calculate_order_fee(req, quantity);
    }
    return whole;
}

} // namespace adapters
