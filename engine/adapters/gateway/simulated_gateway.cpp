#include "adapters/gateway/simulated_gateway.hpp"
#include "adapters/market_data/data_loader.hpp"
#include "model/expiry.hpp"
#include "model/symbol.hpp"
#include "model/utility.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <utility>

namespace adapters {

SimulatedGateway::SimulatedGateway(const model::Portfolios& portfolios,
                                   std::vector<std::string> files, std::string account_file,
                                   Clock clock)
    // Each run's ids its own: an id seen in an earlier run's records is a fill the engine drops.
    : BacktestGateway(portfolios, {.fee_scale = 1.0}, // fees as IB charges them
                      std::format("sim{}", std::chrono::duration_cast<std::chrono::seconds>(
                                               std::chrono::system_clock::now().time_since_epoch())
                                               .count())),
      files_(std::move(files)), account_file_(std::move(account_file)), clock_(std::move(clock)) {
    load_account();
}

void SimulatedGateway::connect() {
    if (!booking_) {
        // Every fill passes through the account on its way to the engine.
        TradeCallback engine = on_trade_;
        on_trade_ = [this, engine](const model::Fill& t) {
            const double sign = t.side == model::Side::BUY ? 1.0 : -1.0;
            book(t.symbol, sign * t.quantity, t.price);
            if (engine) {
                engine(t);
            }
        };
        booking_ = true;
    }
    connected_ = true;
}

auto SimulatedGateway::send_order(const model::OrderRequest& req) -> std::string {
    if (!connected_) {
        return {};
    }
    std::string order_id = BacktestGateway::send_order(req);
    execute_pending_orders();
    return order_id;
}

void SimulatedGateway::restore_orders(std::span<const model::Order> orders) {
    for (model::Order order : orders) {
        order.status = model::Status::CANCELLED;
        put_order(order);
    }
}

void SimulatedGateway::on_minute() {
    execute_pending_orders();
    settle_expired();
    const std::lock_guard lock(mutex_);
    for (auto& [symbol, position] : positions_) {
        if (position.quantity != 0.0) {
            if (const double m = mid(symbol); m > 0.0) {
                marks_[symbol] = m;
            }
        }
    }
}

auto SimulatedGateway::mid(const std::string& symbol) const -> double {
    const auto [bid, ask] = get_market_bid_ask(symbol);
    if (bid > 0.0 && ask > 0.0) {
        return (bid + ask) / 2.0;
    }
    return std::max(bid, ask);
}

void SimulatedGateway::book(const std::string& symbol, double quantity, double price) {
    const std::lock_guard lock(mutex_);
    const double size = portfolios_.multiplier(symbol);
    const double value = quantity * price * size;
    cash_ -= value;
    // The fees the fills so far were charged.
    cash_ -= cumulative_fees() - fees_booked_;
    fees_booked_ = cumulative_fees();

    Position& p = positions_[symbol];
    const double unit = price * size;
    if (p.quantity == 0.0 || (p.quantity > 0.0) == (quantity > 0.0)) {
        p.cost += quantity * unit;
        p.quantity += quantity;
    } else {
        const double side = p.quantity > 0.0 ? 1.0 : -1.0;
        const double closed = std::min(std::abs(quantity), std::abs(p.quantity));
        const double average = p.cost / p.quantity;
        p.realized += side * closed * (unit - average);
        p.cost -= side * closed * average;
        p.quantity -= side * closed;
        if (const double rest = std::abs(quantity) - closed; rest > 0.0) {
            p.quantity = (quantity > 0.0 ? 1.0 : -1.0) * rest;
            p.cost = p.quantity * unit;
        }
    }
    if (std::abs(p.quantity) < 1e-9) {
        p.quantity = 0.0;
        p.cost = 0.0;
    }
    marks_[symbol] = price;
    save_account();
}

void SimulatedGateway::settle_expired() {
    std::vector<std::pair<std::string, double>> held;
    {
        const std::lock_guard lock(mutex_);
        for (const auto& [symbol, p] : positions_) {
            if (p.quantity != 0.0) {
                held.emplace_back(symbol, p.quantity);
            }
        }
    }
    const model::DateTime now = clock_();
    for (const auto& [symbol, quantity] : held) {
        const std::optional<model::OptionId> id = model::parse_option_symbol(symbol);
        const model::Portfolio* portfolio = portfolios_.portfolio_of(symbol);
        if (!id || portfolio == nullptr || !portfolio->underlying() ||
            now < portfolio->calendar().expiry_time(id->expiry)) {
            continue;
        }
        const std::string& underlying = portfolio->underlying()->symbol();
        const double spot = mid(underlying);
        if (spot <= 0.0) {
            continue; // no price at the close yet
        }
        // As the engine books it: closed, and share-settled in the money, delivered at the strike.
        const model::ExpirySettlement settled = model::settle_at_expiry(
            *id, spot, portfolio->physical(), portfolios_.multiplier(symbol));
        book(symbol, -quantity, settled.close);
        if (settled.shares != 0.0) {
            book(underlying, quantity * settled.shares, id->strike);
        }
    }
}

auto SimulatedGateway::query_account() -> std::future<core::BrokerAccount> {
    core::BrokerAccount account;
    account.account = "SIMULATED";
    {
        const std::lock_guard lock(mutex_);
        double market = 0.0;
        double unrealized = 0.0;
        double realized = 0.0;
        for (const auto& [symbol, p] : positions_) {
            realized += p.realized;
            if (p.quantity == 0.0) {
                continue;
            }
            const auto mark = marks_.find(symbol);
            const double price = mark != marks_.end() ? mark->second : 0.0;
            const double size = portfolios_.multiplier(symbol);
            const double value = p.quantity * price * size;
            market += value;
            unrealized += value - p.cost;
            account.positions.push_back({.symbol = symbol,
                                         .quantity = p.quantity,
                                         .market_price = price,
                                         .market_value = value,
                                         .average_cost = p.cost / p.quantity / size,
                                         .unrealized_pnl = value - p.cost,
                                         .realized_pnl = p.realized});
        }
        auto money = [&](std::string key, double v) {
            account.values.push_back({std::move(key), std::format("{:.2f}", v), "USD"});
        };
        money("NetLiquidation", cash_ + market);
        money("TotalCashValue", cash_);
        money("UnrealizedPnL", unrealized);
        money("RealizedPnL", realized);
        account.values.push_back({"AccountType", "simulated", ""});
    }
    account.updated = std::format("{:%H:%M}", std::chrono::floor<std::chrono::minutes>(clock_()));
    std::promise<core::BrokerAccount> done;
    done.set_value(std::move(account));
    return done.get_future();
}

auto SimulatedGateway::sync_contracts(std::string_view portfolio, int /*max_dte*/)
    -> std::future<std::vector<model::Contract>> {
    std::promise<std::vector<model::Contract>> done;
    done.set_value(contracts_of_files(files_, portfolio));
    return done.get_future();
}

void SimulatedGateway::load_account() {
    std::ifstream in(account_file_);
    if (!in) {
        return; // a new account
    }
    try {
        const nlohmann::json j = nlohmann::json::parse(in);
        cash_ = j.value("cash", kStartingCash);
        for (const auto& [symbol, p] : j.value("positions", nlohmann::json::object()).items()) {
            positions_[symbol] = {.quantity = p.value("quantity", 0.0),
                                  .cost = p.value("cost", 0.0),
                                  .realized = p.value("realized", 0.0)};
            marks_[symbol] = p.value("mark", 0.0);
        }
    } catch (const std::exception&) {
        cash_ = kStartingCash; // unreadable: start again
        positions_.clear();
    }
}

void SimulatedGateway::save_account() const {
    nlohmann::json positions = nlohmann::json::object();
    for (const auto& [symbol, p] : positions_) {
        const auto mark = marks_.find(symbol);
        positions[symbol] = {{"quantity", p.quantity},
                             {"cost", p.cost},
                             {"realized", p.realized},
                             {"mark", mark != marks_.end() ? mark->second : 0.0}};
    }
    const std::string written = account_file_ + ".new";
    {
        std::ofstream out(written);
        out << nlohmann::json{{"cash", cash_}, {"positions", positions}}.dump(1);
    }
    std::filesystem::rename(written, account_file_);
}

} // namespace adapters
