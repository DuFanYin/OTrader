#pragma once

/**
 * MarketDataEngine (live): portfolios + contracts，由 load_contracts 两段 + finalize_all_chains
 * 建立。 行情由 Snapshot 事件 → apply_frame。process_option / process_underlying 无 product 分支。
 */

#include "../../core/engine_log.hpp"
#include "../../utilities/base_engine.hpp"
#include "../../utilities/event.hpp"
#include "../../utilities/object.hpp"
#include "../../utilities/portfolio.hpp"
#include <atomic>
#include <chrono>
#include <memory>
#include <set>
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

class MarketDataEngine : public utilities::BaseEngine {
  public:
    explicit MarketDataEngine(utilities::MainEngine* main_engine);

    /** Create all portfolios from hardcoded list; call before load_contracts. */
    void ensure_portfolios_created();
    void process_option(const utilities::ContractData& contract);
    void process_underlying(const utilities::ContractData& contract);
    void finalize_all_chains();

    void subscribe_chains(const std::string& strategy_name,
                          const std::vector<std::string>& chain_symbols);
    void unsubscribe_chains(const std::string& strategy_name);

    utilities::PortfolioData* get_portfolio(const std::string& portfolio_name);
    std::vector<std::string> get_all_portfolio_names() const;
    const utilities::ContractData* get_contract(const std::string& symbol) const;
    std::vector<utilities::ContractData> get_all_contracts() const;

    void set_tradier_config(std::string base_url, std::string token);
    void set_tradier_rate_limit(int requests_per_minute);

    void start_market_data_update();
    void stop_market_data_update();

    /** Parse Tradier chain response (same logic as Python engine_data._fetch_option_chain_ticks +
     * inject_option_chain_market_data), build PortfolioSnapshot, emit Snapshot event.
     * chain_key e.g. "SPXW_20251024"; options = raw API option list; quote_bid/quote_ask for
     * underlying. */
    void inject_tradier_chain(const std::string& chain_key,
                              const std::vector<TradierOptionRaw>& options, double quote_bid,
                              double quote_ask);

  private:
    utilities::PortfolioData* get_or_create_portfolio(const std::string& portfolio_name);
    void process_contract(const utilities::ContractData& contract, bool is_option);
    void write_log(const std::string& msg, int level = INFO);
    void poll_market_data_loop();
    std::vector<std::string> get_fixed_chains_to_query() const;

    std::unordered_map<std::string, std::unique_ptr<utilities::PortfolioData>> portfolios_;
    std::unordered_map<std::string, utilities::ContractData> contracts_;
    std::unordered_map<std::string, std::set<std::string>> active_chains_;
    std::unordered_map<std::string, std::set<std::string>> strategy_chains_;
    std::string tradier_base_url_;
    std::string tradier_token_;
    int tradier_requests_per_minute_ = 60;
    int tradier_requests_used_ = 0;
    std::chrono::steady_clock::time_point tradier_window_start_{};
    std::atomic<bool> started_{false};
    std::thread poll_thread_;
};

} // namespace engines
