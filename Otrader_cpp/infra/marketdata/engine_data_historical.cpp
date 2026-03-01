#include "engine_data_historical.hpp"
#include "engine_main.hpp"
#include "event.hpp"
#include "object.hpp"
#include "occ_utils.hpp"
#include <algorithm>
#include <arrow/api.h>
#include <cctype>
#include <chrono>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <unordered_map>
#include <unordered_set>

namespace backtest {

namespace {

using namespace arrow;

template <typename T> auto get_double(const Array* arr, int64_t i) -> T {
    if (!arr || arr->IsNull(i)) {
        return 0.0;
    }
    return static_cast<T>(static_cast<const DoubleArray*>(arr)->Value(i));
}
template <typename T> auto get_int64(const Array* arr, int64_t i) -> T {
    if (!arr || arr->IsNull(i)) {
        return 0;
    }
    return static_cast<T>(static_cast<const Int64Array*>(arr)->Value(i));
}
auto get_symbol(const Array* arr_sym, int64_t i) -> std::string {
    if ((arr_sym == nullptr) || arr_sym->type_id() != Type::STRING || arr_sym->IsNull(i)) {
        return {};
    }
    return static_cast<const StringArray*>(arr_sym)->GetString(i);
}

} // namespace

BacktestDataEngine::BacktestDataEngine(MainEngine* main_engine)
    : main_engine_(main_engine), loader_(make_parquet_loader()) {}

void BacktestDataEngine::set_risk_free_rate(double rate) {
    if (std::isfinite(rate)) {
        risk_free_rate_ = rate;
    }
    if (portfolio_data_) {
        portfolio_data_->set_risk_free_rate(risk_free_rate_);
    }
}

void BacktestDataEngine::set_iv_price_mode(std::string mode) {
    std::ranges::transform(mode, mode.begin(), [](unsigned char c) -> char {
        return static_cast<char>(std::tolower(c));
    });
    if (mode == "mid" || mode == "bid" || mode == "ask") {
        iv_price_mode_ = std::move(mode);
    }
    if (portfolio_data_) {
        portfolio_data_->set_iv_price_mode(iv_price_mode_);
    }
}

void BacktestDataEngine::load_parquet(std::string const& rel_path, std::string const& time_column,
                                      std::string const& underlying_symbol) {
    loaded_ = false;
    portfolio_ = std::nullopt;
    portfolio_data_.reset();
    occ_to_standard_symbol_.clear();
    option_apply_index_.clear();
    occ_to_option_.clear();
    time_column_ = time_column;
    underlying_symbol_ = underlying_symbol;
    if (underlying_symbol_.empty()) {
        underlying_symbol_ = infer_underlying_from_filename(rel_path);
    }

    if (!loader_->load(rel_path, time_column)) {
        return;
    }

    loaded_ = true;
    DataMeta m = loader_->get_meta();
    if (m.row_count <= 0) {
        return;
    }

    // Derive a reference "today" from the first timestamp string (UTC date at 00:00),
    // so DTE / chain selection in backtest is relative to data start rather than wall-clock now.
    std::optional<utilities::DateTime> dte_ref;
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
            if (main_engine_ != nullptr) {
                main_engine_->write_log("Backtest DTE parse failed from ts_start; using default",
                                        30);
            }
        }
    }

    std::unordered_set<std::string> symbols_set;
    loader_->collect_symbols(symbols_set);
    std::vector<std::string> symbols;
    symbols.reserve(symbols_set.size());
    for (const auto& s : symbols_set) {
        symbols.push_back(s);
    }
    std::ranges::sort(symbols);
    build_portfolio_from_symbols(symbols);

    if (main_engine_ != nullptr) {
        create_portfolio_data(symbols, dte_ref);
        portfolio_data_->finalize_chains();
        build_option_apply_index();
        build_occ_to_option(symbols_set);
        portfolio_data_->set_risk_free_rate(risk_free_rate_);
        portfolio_data_->set_iv_price_mode(iv_price_mode_);
        precompute_snapshots();
    }
}

auto BacktestDataEngine::get_meta() const -> DataMeta {
    if (loader_) {
        return loader_->get_meta();
    }
    return {};
}

void BacktestDataEngine::build_option_apply_index() {
    option_apply_index_.clear();
    if (!portfolio_data_) {
        return;
    }
    const auto& order = portfolio_data_->option_apply_order();
    for (size_t i = 0; i < order.size(); ++i) {
        option_apply_index_[order[i]] = i;
    }
}

void BacktestDataEngine::build_occ_to_option(std::unordered_set<std::string> const& occ_symbols) {
    occ_to_option_.clear();
    if (!portfolio_data_) {
        return;
    }
    const std::string under = underlying_symbol_.empty() ? "UNKNOWN" : underlying_symbol_;
    for (std::string const& occ_sym : occ_symbols) {
        if (occ_sym.empty()) {
            continue;
        }
        std::string std_sym;
        auto occ_it = occ_to_standard_symbol_.find(occ_sym);
        if (occ_it != occ_to_standard_symbol_.end()) {
            std_sym = occ_it->second;
        } else {
            auto [expiry, strike, opt_type] = parse_occ_symbol(occ_sym);
            if (!expiry || !strike || !opt_type) {
                continue;
            }
            std::string expiry_str = format_expiry_yyyymmdd(*expiry);
            std::string opt_type_str = (*opt_type == utilities::OptionType::CALL) ? "CALL" : "PUT";
            int multiplier = 100;
            std_sym = under;
            std_sym += "-";
            std_sym += expiry_str;
            std_sym += "-";
            std_sym += opt_type_str;
            std_sym += "-";
            std_sym += std::to_string(static_cast<int>(*strike));
            std_sym += "-";
            std_sym += std::to_string(multiplier);
            occ_to_standard_symbol_[occ_sym] = std_sym;
        }
        auto opt_it = portfolio_data_->options.find(std_sym);
        if (opt_it == portfolio_data_->options.end()) {
            continue;
        }
        occ_to_option_[occ_sym] = &opt_it->second;
    }
}

void BacktestDataEngine::iter_timesteps(
    std::function<bool(Timestamp, TimestepFrameColumnar const&)> const& fn) const {
    if (!loader_ || !loaded_) {
        return;
    }
    loader_->iter_timesteps(
        [&fn](TimestepFrameColumnar const& frame) -> bool { return fn(frame.timestamp, frame); });
}

auto BacktestDataEngine::build_snapshot_from_frame(TimestepFrameColumnar const& frame,
                                                   utilities::PortfolioSnapshot const* prev)
    -> utilities::PortfolioSnapshot {
    utilities::PortfolioSnapshot snapshot;
    if (frame.num_rows <= 0 || !portfolio_data_) {
        return snapshot;
    }
    const size_t n_opt = portfolio_data_->option_apply_order().size();
    snapshot.portfolio_name = portfolio_data_->name;
    snapshot.datetime = frame.timestamp;
    snapshot.bid.resize(n_opt, 0.0);
    snapshot.ask.resize(n_opt, 0.0);
    snapshot.last.resize(n_opt, 0.0);
    snapshot.delta.resize(n_opt, 0.0);
    snapshot.gamma.resize(n_opt, 0.0);
    snapshot.theta.resize(n_opt, 0.0);
    snapshot.vega.resize(n_opt, 0.0);
    snapshot.iv.resize(n_opt, 0.0);

    if ((prev != nullptr) && prev->bid.size() == n_opt) {
        snapshot.bid = prev->bid;
        snapshot.ask = prev->ask;
        snapshot.last = prev->last;
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
        auto opt_it = occ_to_option_.find(symbol);
        if (opt_it == occ_to_option_.end()) {
            continue;
        }
        utilities::OptionData* opt = opt_it->second;
        auto idx_it = option_apply_index_.find(opt);
        if (idx_it == option_apply_index_.end()) {
            continue;
        }
        const size_t idx = idx_it->second;
        const double bid = ((frame.arr_bid_px != nullptr) && !frame.arr_bid_px->IsNull(i))
                               ? get_double<double>(frame.arr_bid_px, i)
                               : 0.0;
        const double ask = ((frame.arr_ask_px != nullptr) && !frame.arr_ask_px->IsNull(i))
                               ? get_double<double>(frame.arr_ask_px, i)
                               : 0.0;
        snapshot.bid[idx] = bid;
        snapshot.ask[idx] = ask;
        snapshot.last[idx] = (bid > 0.0 && ask > 0.0) ? 0.5 * (bid + ask) : (bid > 0.0 ? bid : ask);
    }

    snapshot.underlying_bid = u_bid;
    snapshot.underlying_ask = u_ask;
    snapshot.underlying_last =
        (u_bid > 0.0 && u_ask > 0.0) ? 0.5 * (u_bid + u_ask) : (u_bid > 0.0 ? u_bid : u_ask);
    return snapshot;
}

void BacktestDataEngine::precompute_snapshots() {
    snapshots_.clear();
    if (!loader_ || !loaded_ || !portfolio_data_) {
        return;
    }
    loader_->iter_timesteps([this](TimestepFrameColumnar const& frame) -> bool {
        utilities::PortfolioSnapshot const* prev =
            snapshots_.empty() ? nullptr : &snapshots_.back();
        snapshots_.push_back(build_snapshot_from_frame(frame, prev));
        return true;
    });
}

void BacktestDataEngine::apply_precomputed_snapshot(size_t i) {
    if (portfolio_data_ && i < snapshots_.size()) {
        portfolio_data_->apply_frame(snapshots_.at(i));
    }
}

void BacktestDataEngine::build_portfolio_from_symbols(std::vector<std::string> const& symbols) {
    BacktestPortfolio p;
    p.underlying = UnderlyingSnapshot();
    for (auto const& s : symbols) {
        OptionSnapshot opt;
        opt.symbol = s;
        p.options[s] = std::move(opt);
    }
    portfolio_ = std::move(p);
}

void BacktestDataEngine::create_portfolio_data(std::vector<std::string> const& symbols,
                                               std::optional<utilities::DateTime> dte_ref) {
    std::string under = underlying_symbol_.empty() ? "UNKNOWN" : underlying_symbol_;
    // 组合名统一为 "backtest"，避免依赖文件名 / 标的推断
    std::string name = "backtest";
    portfolio_data_ = std::make_unique<utilities::PortfolioData>(name);
    if (dte_ref.has_value()) {
        portfolio_data_->set_dte_ref(*dte_ref);
    }
    if (main_engine_ != nullptr) {
        main_engine_->register_portfolio(portfolio_data_.get());
    }

    utilities::ContractData underlying_contract;
    underlying_contract.gateway_name = "BacktestData";
    underlying_contract.symbol = under;
    underlying_contract.exchange = utilities::Exchange::LOCAL;
    underlying_contract.name = under;
    underlying_contract.product = utilities::Product::INDEX;
    underlying_contract.size = 1.0;
    underlying_contract.pricetick = 0.01;
    portfolio_data_->set_underlying(underlying_contract);
    if (main_engine_ != nullptr) {
        main_engine_->register_contract(underlying_contract);
    }

    int option_count = 0;
    for (auto const& sym : symbols) {
        auto [expiry, strike, opt_type] = parse_occ_symbol(sym);
        if (!expiry || !strike || !opt_type) {
            continue;
        }
        std::string expiry_str = format_expiry_yyyymmdd(*expiry);
        std::string opt_type_str = (*opt_type == utilities::OptionType::CALL) ? "CALL" : "PUT";
        int multiplier = 100;
        std::string standard_symbol = under;
        standard_symbol += "-";
        standard_symbol += expiry_str;
        standard_symbol += "-";
        standard_symbol += opt_type_str;
        standard_symbol += "-";
        standard_symbol += std::to_string(static_cast<int>(*strike));
        standard_symbol += "-";
        standard_symbol += std::to_string(multiplier);

        utilities::ContractData option_contract;
        option_contract.gateway_name = "BacktestData";
        option_contract.symbol = standard_symbol;
        option_contract.exchange = utilities::Exchange::LOCAL;
        option_contract.name = sym;
        option_contract.product = utilities::Product::OPTION;
        option_contract.size = static_cast<double>(multiplier);
        option_contract.pricetick = 0.01;
        option_contract.option_strike = strike;
        option_contract.option_type = opt_type;
        option_contract.option_expiry = expiry;
        option_contract.option_underlying = under;
        option_contract.option_index = std::to_string(static_cast<int>(strike.value()));
        portfolio_data_->add_option(option_contract);
        if (main_engine_ != nullptr) {
            main_engine_->register_contract(option_contract);
        }
        occ_to_standard_symbol_[sym] = standard_symbol;
        option_count++;
    }
}

} // namespace backtest
