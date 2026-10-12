#include "engine_data_historical.hpp"
#include "../../utilities/thread_pool.hpp"
#include "../../model/event.hpp"
#include "../../model/object.hpp"
#include "occ_utils.hpp"
#include "../../model/symbol.hpp"
#include "../../model/portfolios.hpp"
#include "../../model/utility.hpp"
#include <algorithm>
#include <arrow/api.h>
#include <cctype>
#include <chrono>
#include <cmath>
#include <concepts>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace adapters {

namespace {

using namespace arrow;

template <std::floating_point T> auto get_double(const Array* arr, int64_t i) -> T {
    if (!arr || arr->IsNull(i)) {
        return T{0};
    }
    return static_cast<T>(static_cast<const DoubleArray*>(arr)->Value(i));
}
template <std::integral T> auto get_int64(const Array* arr, int64_t i) -> T {
    if (!arr || arr->IsNull(i)) {
        return T{0};
    }
    return static_cast<T>(static_cast<const Int64Array*>(arr)->Value(i));
}
auto get_symbol(const Array* arr_sym, int64_t i) -> std::string {
    return std::string(adapters::detail::StringAt(arr_sym, i));
}

} // namespace

BacktestDataEngine::BacktestDataEngine(utilities::Logger* logger,
                                       model::PortfolioStructure* portfolios)
    : utilities::BaseEngine(logger, "BacktestDataEngine"), portfolios_(portfolios) {}

void BacktestDataEngine::set_risk_free_rate(double rate) {
    if (std::isfinite(rate)) {
        risk_free_rate_ = rate;
    }
    if (auto* port = portfolio_data()) {
        port->set_risk_free_rate(risk_free_rate_);
    }
}

void BacktestDataEngine::set_iv_price_mode(std::string mode) {
    std::ranges::transform(mode, mode.begin(), [](unsigned char c) -> char {
        return static_cast<char>(std::tolower(c));
    });
    if (mode == "mid" || mode == "bid" || mode == "ask") {
        iv_price_mode_ = std::move(mode);
    }
    if (auto* port = portfolio_data()) {
        port->set_iv_price_mode(iv_price_mode_);
    }
}

void BacktestDataEngine::load_parquet(std::string const& rel_path, std::string const& time_column,
                                      std::string const& underlying_symbol) {
    loaded_ = false;
    if (portfolios_ != nullptr) {
        portfolios_->clear();
    }
    occ_columns_.clear();
    time_column_ = time_column;
    // The day's options' root names its portfolio: one the live engine trades (SPY, SPXW) is set
    // up as live sets it up (its underlying, how it settles, what it hedges with).
    portfolio_name_ = underlying_symbol.empty() ? infer_underlying_from_filename(rel_path)
                                                : underlying_symbol;
    if (portfolio_name_.empty()) {
        portfolio_name_ = "UNKNOWN";
    }

    if (!loader_.load(rel_path, time_column)) {
        write_log("Failed to load " + rel_path + ": " + loader_.last_error(), utilities::ERROR);
        return;
    }

    loaded_ = true;
    DataMeta m = loader_.get_meta();
    if (m.row_count <= 0) {
        return;
    }

    // Derive a reference "today" from the first timestamp string (UTC date at 00:00),
    // so DTE / chain selection in backtest is relative to data start rather than wall-clock now.
    std::optional<model::DateTime> dte_ref;
    if (!m.ts_start.empty() && m.ts_start.size() >= 10) {
        try {
            std::string d = m.ts_start.substr(0, 10); // "YYYY-MM-DD"
            int year = std::stoi(d.substr(0, 4));
            int month = std::stoi(d.substr(5, 2));
            int day = std::stoi(d.substr(8, 2));
            std::tm tm_utc{};
            tm_utc.tm_year = year - 1900;
            tm_utc.tm_mon = month - 1;
            tm_utc.tm_mday = day;
            tm_utc.tm_hour = 0;
            tm_utc.tm_min = 0;
            tm_utc.tm_sec = 0;
            tm_utc.tm_isdst = 0;
            std::time_t t = 0;
#ifdef _WIN32
            t = _mkgmtime(&tm_utc);
#else
            t = timegm(&tm_utc);
#endif
            if (t != -1) {
                dte_ref = std::chrono::system_clock::from_time_t(t);
            }
        } catch (...) {
            write_log("Backtest DTE parse failed from ts_start; using default", utilities::WARNING);
        }
    }

    std::unordered_set<std::string> symbols_set;
    loader_.collect_symbols(symbols_set);
    std::vector<std::string> symbols;
    symbols.reserve(symbols_set.size());
    std::ranges::copy(symbols_set, std::back_inserter(symbols));
    std::ranges::sort(symbols);

    if (portfolios_ != nullptr) {
        create_portfolio_data(symbols, dte_ref);
        build_occ_columns(symbols_set);
        if (auto* port = portfolio_data()) {
            port->set_risk_free_rate(risk_free_rate_);
            port->set_iv_price_mode(iv_price_mode_);
        }
        precompute_snapshots();
    }
}

auto BacktestDataEngine::get_meta() const -> DataMeta { return loader_.get_meta(); }

void BacktestDataEngine::build_occ_columns(std::unordered_set<std::string> const& occ_symbols) {
    occ_columns_.clear();
    model::PortfolioData* port = portfolio_data();
    if (!port) {
        return;
    }
    std::unordered_map<const model::OptionData*, size_t> column;
    const auto& order = port->option_apply_order();
    for (size_t i = 0; i < order.size(); ++i) {
        column[order[i]] = i;
    }
    for (std::string const& occ_sym : occ_symbols) {
        const std::optional<model::OptionId> id = model::parse_occ_code(portfolio_name_, occ_sym);
        if (!id) {
            continue;
        }
        const auto opt = port->options.find(model::option_symbol(*id));
        if (opt == port->options.end()) {
            continue;
        }
        if (const auto c = column.find(&opt->second); c != column.end()) {
            occ_columns_[occ_sym] = c->second;
        }
    }
}

auto BacktestDataEngine::build_snapshot_from_frame(TimestepFrameColumnar const& frame,
                                                   model::PortfolioSnapshot const* prev)
    -> model::PortfolioSnapshot {
    model::PortfolioSnapshot snapshot;
    model::PortfolioData* port = portfolio_data();
    if (frame.num_rows <= 0 || !port) {
        return snapshot;
    }
    const size_t n_opt = port->option_apply_order().size();
    snapshot.portfolio_name = port->name;
    snapshot.datetime = frame.timestamp;
    snapshot.bid.resize(n_opt, 0.0);
    snapshot.ask.resize(n_opt, 0.0);
    snapshot.mark.resize(n_opt, 0.0);
    snapshot.delta.resize(n_opt, 0.0);
    snapshot.gamma.resize(n_opt, 0.0);
    snapshot.theta.resize(n_opt, 0.0);
    snapshot.vega.resize(n_opt, 0.0);
    snapshot.iv.resize(n_opt, 0.0);

    if ((prev != nullptr) && prev->bid.size() == n_opt) {
        snapshot.bid = prev->bid;
        snapshot.ask = prev->ask;
        snapshot.mark = prev->mark;
    }

    double u_bid = 0.0;
    double u_ask = 0.0;
    for (int64_t r = 0; r < frame.num_rows; ++r) {
        const int64_t i = frame.row_index(r);
        if ((frame.arr_underlying_bid_px != nullptr) && !frame.arr_underlying_bid_px->IsNull(i)) {
            u_bid = get_double<double>(frame.arr_underlying_bid_px, i);
        }
        if ((frame.arr_underlying_ask_px != nullptr) && !frame.arr_underlying_ask_px->IsNull(i)) {
            u_ask = get_double<double>(frame.arr_underlying_ask_px, i);
        }
        std::string symbol = get_symbol(frame.arr_sym, i);
        if (symbol.empty()) {
            continue;
        }
        const auto column = occ_columns_.find(symbol);
        if (column == occ_columns_.end()) {
            continue;
        }
        const size_t idx = column->second;
        const double bid = ((frame.arr_bid_px != nullptr) && !frame.arr_bid_px->IsNull(i))
                               ? get_double<double>(frame.arr_bid_px, i)
                               : 0.0;
        const double ask = ((frame.arr_ask_px != nullptr) && !frame.arr_ask_px->IsNull(i))
                               ? get_double<double>(frame.arr_ask_px, i)
                               : 0.0;
        snapshot.bid[idx] = bid;
        snapshot.ask[idx] = ask;
        snapshot.mark[idx] = model::mark(bid, ask);
    }

    snapshot.underlying_bid = u_bid;
    snapshot.underlying_ask = u_ask;
    snapshot.underlying_mark = model::mark(u_bid, u_ask);
    return snapshot;
}

void BacktestDataEngine::precompute_snapshots() {
    snapshots_.clear();
    if (!loaded_ || !portfolio_data()) {
        return;
    }
    loader_.iter_timesteps([this](TimestepFrameColumnar const& frame) -> bool {
        model::PortfolioSnapshot const* prev = snapshots_.empty() ? nullptr : &snapshots_.back();
        snapshots_.push_back(build_snapshot_from_frame(frame, prev));
        return true;
    });
}

void BacktestDataEngine::price_precomputed_snapshots() {
    const model::PortfolioData* port = portfolio_data();
    if (port == nullptr) {
        return;
    }
    utilities::shared_pool().parallel_for(
        0, snapshots_.size(), [&](size_t i) { port->price_snapshot(snapshots_[i]); },
        /*inline_threshold=*/1);
}

void BacktestDataEngine::apply_precomputed_snapshot(size_t i) {
    if (model::PortfolioData* port = portfolio_data(); port && i < snapshots_.size()) {
        port->apply_frame(snapshots_.at(i));
    }
}

model::PortfolioData* BacktestDataEngine::portfolio_data() const {
    return portfolios_ != nullptr ? portfolios_->get_portfolio(portfolio_name_) : nullptr;
}

void BacktestDataEngine::create_portfolio_data(std::vector<std::string> const& symbols,
                                               std::optional<model::DateTime> dte_ref) {
    model::PortfolioStructure* ps = portfolios_;
    if (ps == nullptr) {
        return;
    }

    const std::string& name = portfolio_name_;
    const model::PortfolioSpec* spec = model::find_portfolio(name);
    const std::string under = spec != nullptr ? std::string(spec->underlying) : name;

    ps->ensure_portfolio(name);
    model::PortfolioData* port = ps->get_portfolio(name);
    if (port == nullptr) {
        return;
    }
    if (dte_ref.has_value()) {
        port->set_dte_ref(*dte_ref);
    }
    if (spec != nullptr) {
        port->physical = spec->physical;
        if (spec->hedge != spec->underlying) {
            port->hedge_symbol = spec->hedge; // its quotes are not in the day: no hedge
        }
    }

    model::ContractData underlying_contract;
    underlying_contract.symbol = under;
    underlying_contract.name = under;
    // One that trades (SPY) is a stock; one hedged with another (SPX) an index.
    underlying_contract.product = spec != nullptr && spec->hedge == spec->underlying
                                      ? model::Product::EQUITY
                                      : model::Product::INDEX;
    underlying_contract.size = 1.0;
    underlying_contract.pricetick = 0.01;
    ps->process_underlying_for_portfolio(name, underlying_contract);

    for (auto const& sym : symbols) {
        const std::optional<model::OptionId> id = model::parse_occ_code(name, sym);
        if (!id) {
            continue;
        }
        const std::string standard_symbol = model::option_symbol(*id);
        constexpr int multiplier = 100;

        model::ContractData option_contract;
        option_contract.symbol = standard_symbol;
        option_contract.name = sym;
        option_contract.product = model::Product::OPTION;
        option_contract.size = static_cast<double>(multiplier);
        option_contract.pricetick = 0.01;
        option_contract.option_strike = id->strike;
        option_contract.option_type = id->type;
        option_contract.option_expiry = model::option_expiry_time(id->expiry);
        option_contract.option_underlying = under;
        ps->process_option_for_portfolio(name, option_contract);
    }
    ps->finalize_all_chains();
}

} // namespace adapters
