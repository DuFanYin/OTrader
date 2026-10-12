#include "simulation.hpp"

#include "../../model/portfolios.hpp"
#include "../../model/symbol.hpp"
#include "../../model/utility.hpp"
#include "parquet_loader.hpp"
#include <algorithm>
#include <arrow/api.h>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace adapters {

namespace {

/** The portfolio a day's file is of: its folder's (data/<PORTFOLIO>/<PORTFOLIO>-YYYY-MM/...). */
auto portfolio_of_file(const std::string& file) -> std::string {
    return std::filesystem::path(file).parent_path().parent_path().filename().string();
}

/** The underlying a portfolio's options are on: the declared one, else the portfolio's name. */
auto underlying_of(const std::string& portfolio) -> std::string {
    const model::PortfolioSpec* spec = model::find_portfolio(portfolio);
    return spec != nullptr ? std::string(spec->underlying) : portfolio;
}

auto read(const std::string& file) -> ArrowParquetLoader {
    ArrowParquetLoader loader;
    if (!loader.load(file)) {
        throw std::runtime_error(std::format("cannot read {}: {}", file, loader.last_error()));
    }
    return loader;
}

auto double_at(const arrow::Array* array, int64_t row) -> double {
    if (array == nullptr || array->IsNull(row)) {
        return 0.0;
    }
    if (array->type_id() == arrow::Type::DOUBLE) {
        return static_cast<const arrow::DoubleArray*>(array)->Value(row);
    }
    if (array->type_id() == arrow::Type::FLOAT) {
        return static_cast<const arrow::FloatArray*>(array)->Value(row);
    }
    return 0.0;
}

/** What a share or an option contract is worth per unit of price. */
auto contract_size(const std::string& symbol) -> double {
    return model::parse_option_symbol(symbol) ? 100.0 : 1.0;
}

} // namespace

// ── The days' contracts and session ──────────────────────────────────────────────────────────

auto simulated_portfolios(std::span<const std::string> files) -> std::vector<std::string> {
    std::vector<std::string> out;
    for (const std::string& file : files) {
        if (std::string name = portfolio_of_file(file); !std::ranges::contains(out, name)) {
            out.push_back(std::move(name));
        }
    }
    return out;
}

auto simulated_contracts(std::span<const std::string> files, std::string_view portfolio)
    -> std::vector<model::ContractData> {
    const std::string name(portfolio);
    const std::string underlying = underlying_of(name);
    const model::PortfolioSpec* spec = model::find_portfolio(name);

    std::unordered_set<std::string> codes;
    for (const std::string& file : files) {
        if (portfolio_of_file(file) == name) {
            read(file).collect_symbols(codes);
        }
    }

    std::vector<model::ContractData> out;
    model::ContractData under;
    under.symbol = underlying;
    under.name = underlying;
    // One that trades (SPY) is a stock; one hedged with another (SPX) an index.
    under.product = spec != nullptr && spec->hedge == spec->underlying ? model::Product::EQUITY
                                                                       : model::Product::INDEX;
    out.push_back(std::move(under));
    for (const std::string& code : codes) {
        const std::optional<model::OptionId> id = model::parse_occ_code(name, code);
        if (!id) {
            continue;
        }
        model::ContractData option;
        option.symbol = model::option_symbol(*id);
        option.name = code;
        option.product = model::Product::OPTION;
        option.size = 100.0;
        option.trading_class = name;
        option.option_strike = id->strike;
        option.option_type = id->type;
        option.option_expiry = model::option_expiry_time(id->expiry);
        option.option_underlying = underlying;
        out.push_back(std::move(option));
    }
    return out;
}

auto simulated_session(std::span<const std::string> files)
    -> std::pair<model::DateTime, model::DateTime> {
    model::DateTime first = model::DateTime::max();
    model::DateTime last = model::DateTime::min();
    for (const std::string& file : files) {
        read(file).iter_timesteps([&](const TimestepFrameColumnar& frame) {
            first = std::min(first, frame.timestamp);
            last = std::max(last, frame.timestamp);
            return true;
        });
    }
    if (first > last) {
        throw std::runtime_error("the simulated days quote nothing");
    }
    return {first, last};
}

// ── ReplayFeed ───────────────────────────────────────────────────────────────────────────────

ReplayFeed::ReplayFeed(std::vector<std::string> files, Clock clock,
                       std::function<void()> after_minute)
    : files_(std::move(files)), clock_(std::move(clock)), after_minute_(std::move(after_minute)) {
    load();
}

ReplayFeed::~ReplayFeed() { stop(); }

void ReplayFeed::load() {
    for (const std::string& file : files_) {
        const std::string portfolio = portfolio_of_file(file);
        const std::string underlying = underlying_of(portfolio);
        std::unordered_map<std::string, std::string> names; // OCC code → the platform's name
        read(file).iter_timesteps([&](const TimestepFrameColumnar& frame) {
            Minute minute;
            minute.time = frame.timestamp;
            minute.underlying = underlying;
            minute.options.reserve(static_cast<std::size_t>(frame.num_rows));
            for (int64_t r = 0; r < frame.num_rows; ++r) {
                const int64_t row = frame.row_index(r);
                if (r == 0) {
                    minute.underlying_bid = double_at(frame.arr_underlying_bid_px, row);
                    minute.underlying_ask = double_at(frame.arr_underlying_ask_px, row);
                }
                const std::string code(detail::StringAt(frame.arr_sym, row));
                auto it = names.find(code);
                if (it == names.end()) {
                    const std::optional<model::OptionId> id = model::parse_occ_code(portfolio, code);
                    it = names.emplace(code, id ? model::option_symbol(*id) : std::string()).first;
                }
                if (it->second.empty()) {
                    continue;
                }
                minute.options.push_back(it->second);
                minute.bids.push_back(double_at(frame.arr_bid_px, row));
                minute.asks.push_back(double_at(frame.arr_ask_px, row));
            }
            minutes_.push_back(std::move(minute));
            return true;
        });
    }
    std::ranges::stable_sort(minutes_, {}, &Minute::time);
}

void ReplayFeed::start(QuoteBook& book, const utilities::BaseEngine& log) {
    stop();
    {
        const std::lock_guard lock(mutex_);
        stopping_ = false;
    }
    log.write_log(std::format("Simulated quotes: {} minutes from {} files", minutes_.size(),
                              files_.size()),
                  utilities::INFO);
    thread_ = std::thread([this, &book] { run(book); });
}

void ReplayFeed::stop() {
    {
        const std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
}

void ReplayFeed::run(QuoteBook& book) {
    // Each contract the book plans, by name, and the slots its quotes go to.
    std::unordered_map<std::string, std::vector<QuoteSlot>> slots;
    auto plan = [&] {
        for (auto& [_, s] : slots) {
            for (const QuoteSlot& slot : s) {
                book.clear(slot);
            }
        }
        slots.clear();
        for (const Instrument& i : book.instruments(0)) {
            slots[i.contract->symbol].push_back(i.slot);
        }
    };
    plan();
    std::size_t next = 0;
    std::unique_lock lock(mutex_);
    while (!stopping_) {
        lock.unlock();
        if (book.replan_due(std::chrono::steady_clock::now())) {
            plan();
        }
        bool published = false;
        const model::DateTime now = clock_();
        while (next < minutes_.size() && minutes_[next].time <= now) {
            const Minute& m = minutes_[next++];
            if (auto it = slots.find(m.underlying); it != slots.end()) {
                for (const QuoteSlot& slot : it->second) {
                    book.quote(slot, QuoteField::Bid, m.underlying_bid, m.time);
                    book.quote(slot, QuoteField::Ask, m.underlying_ask, m.time);
                }
            }
            for (std::size_t k = 0; k < m.options.size(); ++k) {
                if (auto it = slots.find(m.options[k]); it != slots.end()) {
                    for (const QuoteSlot& slot : it->second) {
                        book.quote(slot, QuoteField::Bid, m.bids[k], m.time);
                        book.quote(slot, QuoteField::Ask, m.asks[k], m.time);
                    }
                }
            }
            published = true;
        }
        if (published) {
            book.publish();
            if (after_minute_) {
                after_minute_();
            }
        }
        lock.lock();
        wake_.wait_for(lock, std::chrono::milliseconds(250), [this] { return stopping_; });
    }
}

// ── SimulatedGateway ─────────────────────────────────────────────────────────────────────────

SimulatedGateway::SimulatedGateway(const model::PortfolioStructure& portfolios,
                                   std::vector<std::string> files, std::string account_file,
                                   Clock clock, double fee_rate, double slippage_bps)
    // Each run's ids its own: an id seen in an earlier run's records is a fill the engine drops.
    : BacktestGateway(portfolios, fee_rate, slippage_bps,
                      std::format("sim{}", std::chrono::duration_cast<std::chrono::seconds>(
                                               std::chrono::system_clock::now().time_since_epoch())
                                               .count())),
      portfolios_(portfolios),
      files_(std::move(files)), account_file_(std::move(account_file)), clock_(std::move(clock)) {
    load_account();
}

void SimulatedGateway::connect() {
    if (!booking_) {
        // Every fill passes through the account on its way to the engine.
        TradeCallback engine = on_trade_;
        on_trade_ = [this, engine](const model::TradeData& t) {
            const double sign = t.direction == model::Direction::LONG ? 1.0 : -1.0;
            book(t.symbol, sign * t.volume, t.price);
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
    std::string orderid = BacktestGateway::send_order(req);
    execute_pending_orders();
    return orderid;
}

void SimulatedGateway::restore_orders(std::span<const model::OrderData> orders) {
    for (model::OrderData order : orders) {
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
    const model::PortfolioData* portfolio = portfolios_.portfolio_of(symbol);
    if (portfolio == nullptr) {
        return 0.0;
    }
    double bid = 0.0;
    double ask = 0.0;
    if (auto it = portfolio->options.find(symbol); it != portfolio->options.end()) {
        bid = it->second.bid_price;
        ask = it->second.ask_price;
    } else if (portfolio->underlying != nullptr && portfolio->underlying->symbol == symbol) {
        bid = portfolio->underlying->bid_price;
        ask = portfolio->underlying->ask_price;
    }
    if (bid > 0.0 && ask > 0.0) {
        return (bid + ask) / 2.0;
    }
    return std::max(bid, ask);
}

void SimulatedGateway::book(const std::string& symbol, double quantity, double price) {
    const std::lock_guard lock(mutex_);
    const double value = quantity * price * contract_size(symbol);
    cash_ -= value;
    // The fees the fills so far were charged.
    cash_ -= cumulative_fees() - fees_booked_;
    fees_booked_ = cumulative_fees();

    Position& p = positions_[symbol];
    const double unit = price * contract_size(symbol);
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
        if (!id || now < model::option_expiry_time(id->expiry)) {
            continue;
        }
        const model::PortfolioData* portfolio = portfolios_.portfolio_of(symbol);
        if (portfolio == nullptr || portfolio->underlying == nullptr) {
            continue;
        }
        const double spot = mid(portfolio->underlying->symbol);
        if (spot <= 0.0) {
            continue; // no price at the close yet
        }
        const bool call = id->type == model::OptionType::CALL;
        const double intrinsic = std::max(0.0, call ? spot - id->strike : id->strike - spot);
        book(symbol, -quantity, intrinsic); // the option closes at its intrinsic value
        const model::PortfolioSpec* spec = model::find_portfolio(portfolio->name);
        if (spec != nullptr && spec->physical && intrinsic >= 0.01) {
            // Delivered: the shares at the underlying's price (with the intrinsic value, the strike).
            book(portfolio->underlying->symbol, quantity * 100.0 * (call ? 1.0 : -1.0), spot);
        }
    }
}

auto SimulatedGateway::query_account() -> std::future<BrokerAccount> {
    BrokerAccount account;
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
            const double size = contract_size(symbol);
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
    std::promise<BrokerAccount> done;
    done.set_value(std::move(account));
    return done.get_future();
}

auto SimulatedGateway::sync_contracts(std::string_view portfolio, int /*max_dte*/)
    -> std::future<std::vector<model::ContractData>> {
    std::promise<std::vector<model::ContractData>> done;
    done.set_value(simulated_contracts(files_, portfolio));
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
