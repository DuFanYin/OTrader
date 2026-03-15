#pragma once

/**
 * MarketDataEngine (live): portfolios + contracts via load_contracts two-phase +
 * finalize_all_chains. Snapshot event → apply_frame. process_option/process_underlying no product
 * branch.
 */

#include "../../core/engine_log.hpp"
#include "../../core/portfolio_structure.hpp"
#include "../../utilities/base_engine.hpp"
#include "../../utilities/event.hpp"
#include "../../utilities/object.hpp"
#include "../../utilities/portfolio.hpp"
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <set>
#include <span>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace engines {

struct MainEngine;

/** Raw option from Tradier API (before symbol formatting). Only bid/ask/last for pricing;
 * Greeks/IV are computed inside portfolio apply_frame. */
struct TradierOptionRaw {
    std::string symbol;      // OCC e.g. SPXW260302C02800000 (for matching)
    std::string root_symbol; // set to root_symbol or underlying from API
    double strike = 0.0;
    std::string option_type; // "call" or "put" -> "C" / "P"
    int contract_size = 100;
    double bid = 0.0;
    double ask = 0.0;
    double last = 0.0;
    double volume = 0.0;
    double open_interest = 0.0;
};

class MarketDataEngine : public utilities::BaseEngine, public PortfolioStructure {
  public:
    explicit MarketDataEngine(utilities::MainEngine* main_engine);

    void subscribe_chains(const std::string& strategy_name,
                          std::span<const std::string> chain_symbols);
    void unsubscribe_chains(const std::string& strategy_name);

    void set_tradier_config(std::string base_url, std::string token);
    void set_tradier_rate_limit(int requests_per_minute);

    void start_market_data_update();
    void stop_market_data_update();

    /** Set callback for snapshot emission. entry_market_data uses this to PUB over ZMQ. Required
     * before start. */
    void set_snapshot_callback(std::function<void(const utilities::PortfolioSnapshot&)> cb) {
        snapshot_callback_ = std::move(cb);
    }

    /** Parse Tradier chain response (same logic as Python engine_data._fetch_option_chain_ticks +
     * inject_option_chain_market_data), build PortfolioSnapshot, emit Snapshot event.
     * chain_key e.g. "SPXW_20251024"; options = raw API option list; quote_bid/quote_ask for
     * underlying. */
    void inject_tradier_chain(const std::string& chain_key,
                              std::span<const TradierOptionRaw> options, double quote_bid,
                              double quote_ask);

  private:
    void poll_market_data_loop(const std::stop_token& st);
    std::vector<std::string> get_fixed_chains_to_query() const;

    std::unordered_map<std::string, std::set<std::string>> active_chains_;
    std::unordered_map<std::string, std::set<std::string>> strategy_chains_;
    std::string tradier_base_url_;
    std::string tradier_token_;
    int tradier_requests_per_minute_ = 60;
    int tradier_requests_used_ = 0;
    std::chrono::steady_clock::time_point tradier_window_start_{};
    std::atomic<bool> started_{false};
    std::jthread poll_thread_;
    std::function<void(const utilities::PortfolioSnapshot&)> snapshot_callback_;
};

} // namespace engines
