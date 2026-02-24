#include "engine_data_historical.hpp"
#include "engine_main.hpp"
#include "occ_utils.hpp"
#include "event.hpp"
#include "object.hpp"
#include "lets_be_rational_api.hpp"
#include <arrow/api.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace backtest {

namespace {

constexpr double kMinVol = 1e-6;
constexpr double kMaxVol = 5.0;
constexpr double kMinT = 1e-6;

double normal_pdf(double x) {
    static constexpr double inv_sqrt_2pi = 0.3989422804014327;
    return inv_sqrt_2pi * std::exp(-0.5 * x * x);
}

double normal_cdf(double x) {
    return 0.5 * (1.0 + std::erf(x / std::sqrt(2.0)));
}

double bs_price(bool is_call, double s, double k, double t, double r, double sigma) {
    if (s <= 0.0 || k <= 0.0 || t <= 0.0 || sigma <= 0.0) return 0.0;
    const double sqrt_t = std::sqrt(t);
    const double d1 = (std::log(s / k) + (r + 0.5 * sigma * sigma) * t) / (sigma * sqrt_t);
    const double d2 = d1 - sigma * sqrt_t;
    const double df = std::exp(-r * t);
    if (is_call) {
        return s * normal_cdf(d1) - k * df * normal_cdf(d2);
    }
    return k * df * normal_cdf(-d2) - s * normal_cdf(-d1);
}

double bs_vega_raw(double s, double k, double t, double r, double sigma) {
    if (s <= 0.0 || k <= 0.0 || t <= 0.0 || sigma <= 0.0) return 0.0;
    const double sqrt_t = std::sqrt(t);
    const double d1 = (std::log(s / k) + (r + 0.5 * sigma * sigma) * t) / (sigma * sqrt_t);
    return s * normal_pdf(d1) * sqrt_t;
}

struct Greeks {
    double delta = 0.0;
    double gamma = 0.0;
    double theta = 0.0;  // per-day, aligned with py_vollib
    double vega = 0.0;   // per-1% vol move, aligned with py_vollib
};

struct GreekWorkItem {
    utilities::OptionData* option = nullptr;
    bool is_call = true;
    double s = 0.0;
    double k = 0.0;
    double t = 0.0;
    double iv = 0.0;
    double size = 100.0;
};

// Per-frame batch: compute IV and Greeks for all rows, then inject once.
struct FrameOptionBatchRow {
    utilities::OptionData* option = nullptr;
    double bid = 0.0, ask = 0.0, s = 0.0, k = 0.0, t = 0.0, iv = 0.0, size = 100.0;
    bool is_call = true;
};

Greeks bs_greeks(bool is_call, double s, double k, double t, double r, double sigma) {
    Greeks g;
    if (s <= 0.0 || k <= 0.0 || t <= 0.0 || sigma <= 0.0) return g;

    const double sqrt_t = std::sqrt(t);
    const double d1 = (std::log(s / k) + (r + 0.5 * sigma * sigma) * t) / (sigma * sqrt_t);
    const double d2 = d1 - sigma * sqrt_t;
    const double pdf = normal_pdf(d1);
    const double df = std::exp(-r * t);

    g.delta = is_call ? normal_cdf(d1) : (normal_cdf(d1) - 1.0);
    g.gamma = pdf / (s * sigma * sqrt_t);
    const double theta_annual = is_call
                                    ? (-(s * pdf * sigma) / (2.0 * sqrt_t) - r * k * df * normal_cdf(d2))
                                    : (-(s * pdf * sigma) / (2.0 * sqrt_t) + r * k * df * normal_cdf(-d2));
    g.theta = theta_annual / 365.0;
    g.vega = bs_vega_raw(s, k, t, r, sigma) / 100.0;
    return g;
}

/** Single-pass batch Greeks into contiguous arrays (enables auto-vectorization). */
void bs_greeks_batch(const std::vector<FrameOptionBatchRow>& batch, double r,
                     std::vector<double>& delta, std::vector<double>& gamma,
                     std::vector<double>& theta, std::vector<double>& vega) {
    const size_t n = batch.size();
    delta.resize(n);
    gamma.resize(n);
    theta.resize(n);
    vega.resize(n);
    for (size_t i = 0; i < n; ++i) {
        Greeks g = bs_greeks(batch[i].is_call, batch[i].s, batch[i].k, batch[i].t, r, batch[i].iv);
        delta[i] = g.delta;
        gamma[i] = g.gamma;
        theta[i] = g.theta;
        vega[i] = g.vega;
    }
}

double years_to_expiry(Timestamp now, const std::optional<utilities::DateTime>& expiry) {
    if (!expiry.has_value()) return 0.0;
    const auto dt = expiry.value() - now;
    const auto secs = std::chrono::duration_cast<std::chrono::seconds>(dt).count();
    if (secs <= 0) return 0.0;
    return std::max(kMinT, static_cast<double>(secs) / (365.25 * 24.0 * 3600.0));
}

double pick_iv_input_price(double bid, double ask, const std::string& mode) {
    if (mode == "bid") return bid > 0.0 ? bid : 0.0;
    if (mode == "ask") return ask > 0.0 ? ask : 0.0;
    return (bid > 0.0 && ask > 0.0) ? (bid + ask) / 2.0 : (bid > 0.0 ? bid : ask);
}

}  // namespace

BacktestDataEngine::BacktestDataEngine(MainEngine* main_engine)
    : main_engine_(main_engine), loader_(make_parquet_loader()) {}

void BacktestDataEngine::set_risk_free_rate(double rate) {
    if (std::isfinite(rate)) risk_free_rate_ = rate;
}

void BacktestDataEngine::set_iv_price_mode(std::string mode) {
    std::transform(
        mode.begin(),
        mode.end(),
        mode.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); }
    );
    if (mode == "mid" || mode == "bid" || mode == "ask") {
        iv_price_mode_ = std::move(mode);
    }
}

void BacktestDataEngine::load_parquet(std::string const& rel_path,
                                      std::string const& time_column,
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
    if (m.row_count <= 0) return;

    std::unordered_set<std::string> symbols_set;
    loader_->collect_symbols(symbols_set);
    std::vector<std::string> symbols;
    symbols.reserve(symbols_set.size());
    for (const auto& s : symbols_set) symbols.push_back(s);
    std::sort(symbols.begin(), symbols.end());
    build_portfolio_from_symbols(symbols);

    if (main_engine_) {
        create_portfolio_data(symbols);
        portfolio_data_->finalize_chains();
        build_option_apply_index();
        build_occ_to_option(symbols_set);
        precompute_snapshots();
    }
}

DataMeta BacktestDataEngine::get_meta() const {
    if (loader_) return loader_->get_meta();
    return {};
}

void BacktestDataEngine::build_option_apply_index() {
    option_apply_index_.clear();
    if (!portfolio_data_) return;
    const auto& order = portfolio_data_->option_apply_order();
    for (size_t i = 0; i < order.size(); ++i)
        option_apply_index_[order[i]] = i;
}

void BacktestDataEngine::build_occ_to_option(std::unordered_set<std::string> const& occ_symbols) {
    occ_to_option_.clear();
    if (!portfolio_data_) return;
    const std::string under = underlying_symbol_.empty() ? "UNKNOWN" : underlying_symbol_;
    for (std::string const& occ_sym : occ_symbols) {
        if (occ_sym.empty()) continue;
        std::string std_sym;
        auto occ_it = occ_to_standard_symbol_.find(occ_sym);
        if (occ_it != occ_to_standard_symbol_.end()) {
            std_sym = occ_it->second;
        } else {
            auto [expiry, strike, opt_type] = parse_occ_symbol(occ_sym);
            if (!expiry || !strike || !opt_type) continue;
            std::string expiry_str = format_expiry_yyyymmdd(*expiry);
            std::string opt_type_str = (*opt_type == utilities::OptionType::CALL) ? "CALL" : "PUT";
            int multiplier = 100;
            std_sym = under + "-" + expiry_str + "-" + opt_type_str + "-" +
                      std::to_string(static_cast<int>(*strike)) + "-" + std::to_string(multiplier);
            occ_to_standard_symbol_[occ_sym] = std_sym;
        }
        auto opt_it = portfolio_data_->options.find(std_sym);
        if (opt_it == portfolio_data_->options.end()) continue;
        occ_to_option_[occ_sym] = &opt_it->second;
    }
}

void BacktestDataEngine::iter_timesteps(
    std::function<bool(Timestamp, TimestepFrameColumnar const&)> const& fn) const {
    if (!loader_ || !loaded_) return;
    loader_->iter_timesteps([&fn](TimestepFrameColumnar const& frame) { return fn(frame.timestamp, frame); });
}

namespace {

using namespace arrow;

template <typename T>
T get_double(const Array* arr, int64_t i) {
    if (!arr || arr->IsNull(i)) return 0.0;
    return static_cast<T>(static_cast<const DoubleArray*>(arr)->Value(i));
}
template <typename T>
T get_int64(const Array* arr, int64_t i) {
    if (!arr || arr->IsNull(i)) return 0;
    return static_cast<T>(static_cast<const Int64Array*>(arr)->Value(i));
}
std::string get_symbol(const Array* arr_sym, int64_t i) {
    if (!arr_sym || arr_sym->type_id() != Type::STRING || arr_sym->IsNull(i)) return {};
    return static_cast<const StringArray*>(arr_sym)->GetString(i);
}

}  // namespace

utilities::PortfolioSnapshot BacktestDataEngine::build_snapshot_from_frame(TimestepFrameColumnar const& frame,
                                                                         utilities::PortfolioSnapshot const* prev) {
    utilities::PortfolioSnapshot snapshot;
    if (frame.num_rows <= 0 || !portfolio_data_) return snapshot;

    double u_bid = 0.0, u_ask = 0.0;
    std::vector<FrameOptionBatchRow> batch;
    batch.reserve(static_cast<size_t>(frame.num_rows));

    for (int64_t r = 0; r < frame.num_rows; ++r) {
        const int64_t i = frame.row_index(r);
        if (frame.arr_underlying_bid_px && !frame.arr_underlying_bid_px->IsNull(i))
            u_bid = get_double<double>(frame.arr_underlying_bid_px, i);
        if (frame.arr_underlying_ask_px && !frame.arr_underlying_ask_px->IsNull(i))
            u_ask = get_double<double>(frame.arr_underlying_ask_px, i);

        std::string symbol = get_symbol(frame.arr_sym, i);
        if (symbol.empty()) continue;

        auto opt_it = occ_to_option_.find(symbol);
        if (opt_it == occ_to_option_.end()) continue;
        utilities::OptionData* opt = opt_it->second;

        double bid = (frame.arr_bid_px && !frame.arr_bid_px->IsNull(i)) ? get_double<double>(frame.arr_bid_px, i) : 0.0;
        double ask = (frame.arr_ask_px && !frame.arr_ask_px->IsNull(i)) ? get_double<double>(frame.arr_ask_px, i) : 0.0;
        const double k = opt->strike_price.value_or(0.0);
        const double t = years_to_expiry(frame.timestamp, opt->option_expiry);
        const double px = pick_iv_input_price(bid, ask, iv_price_mode_);
        const bool is_call = opt->option_type > 0;
        const double size = opt->size;
        if (k <= 0.0 || t <= 0.0 || px <= 0.0) continue;

        batch.push_back(FrameOptionBatchRow{
            .option = opt,
            .bid = bid,
            .ask = ask,
            .s = 0.0,
            .k = k,
            .t = t,
            .size = size,
            .is_call = is_call,
        });
    }

    const double spot = (u_bid > 0.0 || u_ask > 0.0)
                           ? ((u_bid > 0.0 && u_ask > 0.0) ? 0.5 * (u_bid + u_ask) : (u_bid > 0.0 ? u_bid : u_ask))
                           : 0.0;
    std::vector<utilities::OptionData*> zeroed_options;
    for (auto& row : batch) {
        row.s = spot;
        if (row.s <= 0.0) zeroed_options.push_back(row.option);
    }
    batch.erase(std::remove_if(batch.begin(), batch.end(),
                               [](FrameOptionBatchRow const& row) { return row.s <= 0.0; }),
                batch.end());

    const size_t n = batch.size();
    const unsigned int n_workers = std::max(1u, static_cast<unsigned int>(std::thread::hardware_concurrency()));
    if (n > 0) {
        std::vector<std::thread> threads;
        threads.reserve(n_workers);
        const size_t chunk = (n + n_workers - 1) / n_workers;
        for (unsigned int t = 0; t < n_workers; ++t) {
            const size_t start = t * chunk;
            const size_t end = std::min(start + chunk, n);
            if (start >= end) break;
            threads.emplace_back([this, &batch, start, end]() {
                for (size_t i = start; i < end; ++i) {
                    auto& row = batch[i];
                    const double q = row.is_call ? 1.0 : -1.0;
                    const double px = pick_iv_input_price(row.bid, row.ask, iv_price_mode_);
                    double iv = implied_volatility_from_a_transformed_rational_guess(px, row.s, row.k, row.t, q);
                    if (!std::isfinite(iv) || iv <= 0.0) iv = 0.0;
                    if (iv > kMaxVol) iv = kMaxVol;
                    row.iv = iv;
                }
            });
        }
        for (auto& th : threads) th.join();
    }

    std::vector<double> delta, gamma, theta, vega;
    bs_greeks_batch(batch, risk_free_rate_, delta, gamma, theta, vega);

    const double u_mid = (u_bid > 0.0 && u_ask > 0.0) ? 0.5 * (u_bid + u_ask) : (u_bid > 0.0 ? u_bid : u_ask);
    const size_t n_opt = portfolio_data_->option_apply_order().size();
    snapshot.portfolio_name = portfolio_data_->name;
    snapshot.datetime = frame.timestamp;
    snapshot.underlying_bid = u_bid;
    snapshot.underlying_ask = u_ask;
    snapshot.underlying_last = u_mid;
    snapshot.bid.resize(n_opt, 0.0);
    snapshot.ask.resize(n_opt, 0.0);
    snapshot.last.resize(n_opt, 0.0);
    snapshot.delta.resize(n_opt, 0.0);
    snapshot.gamma.resize(n_opt, 0.0);
    snapshot.theta.resize(n_opt, 0.0);
    snapshot.vega.resize(n_opt, 0.0);
    snapshot.iv.resize(n_opt, 0.0);
    if (prev && prev->bid.size() == n_opt) {
        snapshot.bid = prev->bid;
        snapshot.ask = prev->ask;
        snapshot.last = prev->last;
        snapshot.delta = prev->delta;
        snapshot.gamma = prev->gamma;
        snapshot.theta = prev->theta;
        snapshot.vega = prev->vega;
        snapshot.iv = prev->iv;
    }

    for (size_t i = 0; i < batch.size(); ++i) {
        FrameOptionBatchRow const& row = batch[i];
        auto it = option_apply_index_.find(row.option);
        if (it == option_apply_index_.end()) continue;
        const size_t idx = it->second;
        const double last_px = (row.bid > 0.0 && row.ask > 0.0) ? 0.5 * (row.bid + row.ask) : (row.bid > 0.0 ? row.bid : row.ask);
        snapshot.bid[idx] = row.bid;
        snapshot.ask[idx] = row.ask;
        snapshot.last[idx] = last_px;
        snapshot.delta[idx] = delta[i];
        snapshot.gamma[idx] = gamma[i];
        snapshot.theta[idx] = theta[i];
        snapshot.vega[idx] = vega[i];
        snapshot.iv[idx] = row.iv;
    }
    for (utilities::OptionData* opt : zeroed_options) {
        if (!opt) continue;
        auto it = option_apply_index_.find(opt);
        if (it == option_apply_index_.end()) continue;
        const size_t idx = it->second;
        if (prev && idx < prev->bid.size()) {
            snapshot.bid[idx] = prev->bid[idx];
            snapshot.ask[idx] = prev->ask[idx];
            snapshot.last[idx] = prev->last[idx];
        } else {
            snapshot.bid[idx] = opt->bid_price;
            snapshot.ask[idx] = opt->ask_price;
            snapshot.last[idx] = opt->mid_price;
        }
        snapshot.delta[idx] = 0.0;
        snapshot.gamma[idx] = 0.0;
        snapshot.theta[idx] = 0.0;
        snapshot.vega[idx] = 0.0;
        snapshot.iv[idx] = 0.0;
    }

    return snapshot;
}

void BacktestDataEngine::precompute_snapshots() {
    snapshots_.clear();
    if (!loader_ || !loaded_ || !portfolio_data_) return;
    loader_->iter_timesteps([this](TimestepFrameColumnar const& frame) {
        utilities::PortfolioSnapshot const* prev = snapshots_.empty() ? nullptr : &snapshots_.back();
        snapshots_.push_back(build_snapshot_from_frame(frame, prev));
        return true;
    });
}

void BacktestDataEngine::apply_precomputed_snapshot(size_t i) {
    if (portfolio_data_ && i < snapshots_.size())
        portfolio_data_->apply_frame(snapshots_.at(i));
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

void BacktestDataEngine::create_portfolio_data(std::vector<std::string> const& symbols) {
    std::string under = underlying_symbol_.empty() ? "UNKNOWN" : underlying_symbol_;
    // 组合名统一为 "backtest"，避免依赖文件名 / 标的推断
    std::string name = "backtest";
    portfolio_data_ = std::make_unique<utilities::PortfolioData>(name);
    if (main_engine_)
        main_engine_->register_portfolio(portfolio_data_.get());

    utilities::ContractData underlying_contract;
    underlying_contract.gateway_name = "BacktestData";
    underlying_contract.symbol = under;
    underlying_contract.exchange = utilities::Exchange::LOCAL;
    underlying_contract.name = under;
    underlying_contract.product = utilities::Product::INDEX;
    underlying_contract.size = 1.0;
    underlying_contract.pricetick = 0.01;
    portfolio_data_->set_underlying(underlying_contract);
    if (main_engine_)
        main_engine_->register_contract(underlying_contract);

    int option_count = 0;
    for (auto const& sym : symbols) {
        auto [expiry, strike, opt_type] = parse_occ_symbol(sym);
        if (!expiry || !strike || !opt_type) continue;
        std::string expiry_str = format_expiry_yyyymmdd(*expiry);
        std::string opt_type_str = (*opt_type == utilities::OptionType::CALL) ? "CALL" : "PUT";
        int multiplier = 100;
        std::string standard_symbol = under + "-" + expiry_str + "-" + opt_type_str + "-" + std::to_string(static_cast<int>(*strike)) + "-" + std::to_string(multiplier);

        utilities::ContractData option_contract;
        option_contract.gateway_name = "BacktestData";
        option_contract.symbol = standard_symbol;
        option_contract.exchange = utilities::Exchange::LOCAL;
        option_contract.name = sym;
        option_contract.product = utilities::Product::OPTION;
        option_contract.size = static_cast<double>(multiplier);
        option_contract.pricetick = 0.01;
        option_contract.option_strike = *strike;
        option_contract.option_type = *opt_type;
        option_contract.option_expiry = *expiry;
        option_contract.option_underlying = under;
        option_contract.option_index = std::to_string(static_cast<int>(*strike));
        portfolio_data_->add_option(option_contract);
        if (main_engine_)
            main_engine_->register_contract(option_contract);
        occ_to_standard_symbol_[sym] = standard_symbol;
        option_count++;
    }
}

}  // namespace backtest
