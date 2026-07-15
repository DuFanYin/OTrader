#pragma once

/** PortfolioData, ChainData, OptionData, UnderlyingData (from portfolio.py). */

#include "constant.hpp"
#include "event.hpp"
#include "object.hpp"
#include "utility.hpp"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace utilities {

// Forward decls
struct PortfolioData;
struct ChainData;
struct UnderlyingData;

struct OptionData {
    std::string symbol;
    Exchange exchange = Exchange::LOCAL;
    double size = 100.0;
    OptionData() = default;
    explicit OptionData(const ContractData& contract);
    double bid_price = 0.0;
    double ask_price = 0.0;
    double mid_price = 0.0;
    std::optional<TickData> tick;
    PortfolioData* portfolio = nullptr;

    std::optional<double> strike_price;
    std::optional<std::string> chain_index;
    int option_type = 1;
    std::optional<DateTime> option_expiry;
    UnderlyingData* underlying = nullptr;
    ChainData* chain = nullptr;
    double delta = 0;
    double gamma = 0;
    double theta = 0;
    double vega = 0;
    double mid_iv = 0;

    void set_portfolio(PortfolioData* p);
    void set_chain(ChainData* c);
    void set_underlying(UnderlyingData* u);
};

struct UnderlyingData {
    std::string symbol;
    Exchange exchange = Exchange::LOCAL;
    double size = 1.0;
    double bid_price = 0.0;
    double ask_price = 0.0;
    double mid_price = 0.0;
    std::optional<TickData> tick;
    PortfolioData* portfolio = nullptr;
    double theo_delta = 1.0;
    std::unordered_map<std::string, ChainData*> chains;

    UnderlyingData() = default;
    explicit UnderlyingData(const ContractData& contract);
    void set_portfolio(PortfolioData* p);
    void add_chain(ChainData* chain);
    void update_underlying_tick(const TickData& tick_data);
};

struct ChainData {
    std::string chain_symbol;
    UnderlyingData* underlying = nullptr;
    std::unordered_map<std::string, OptionData*> options;
    std::unordered_map<std::string, OptionData*> calls;
    std::unordered_map<std::string, OptionData*> puts;
    PortfolioData* portfolio = nullptr;
    std::vector<std::string> indexes;
    std::unordered_set<std::string> index_set;
    double atm_price = 0;
    std::string atm_index;
    int days_to_expiry = 0;
    double time_to_expiry = 0;

    explicit ChainData(std::string chain_symbol);
    void add_option(OptionData* option);
    /** Sort indexes. */
    void sort_indexes();
    void update_option_chain(const ChainMarketData& market_data);
    void set_underlying(UnderlyingData* u);
    void set_portfolio(PortfolioData* p);
    void calculate_atm_price();
    std::optional<double> get_atm_iv() const;
    static std::optional<double>
    best_iv(const std::unordered_map<std::string, OptionData*>& options_map, double target);
    std::optional<double> get_skew(double delta_target = 25.0) const;
};

struct PortfolioData {
    std::string name;
    std::unordered_map<std::string, OptionData> options;
    std::unordered_map<std::string, std::unique_ptr<ChainData>> chains;
    std::unique_ptr<UnderlyingData> underlying;
    std::string underlying_symbol;
    /** option_apply_order (finalize_chains). */
    std::vector<OptionData*> option_apply_order_;

    double risk_free_rate_ = 0.05;
    std::string iv_price_mode_ = "mid";
    DateTime dte_ref_{};

    explicit PortfolioData(std::string name);
    void set_risk_free_rate(double rate);
    void set_iv_price_mode(std::string mode);
    void set_dte_ref(DateTime ref);
    [[nodiscard]] DateTime dte_ref() const { return dte_ref_; }
    void update_option_chain(const ChainMarketData& market_data);
    void update_underlying_tick(const TickData& tick_data) const;
    /** Apply snapshot: IV/Greeks → underlying + option_apply_order. */
    void apply_frame(const PortfolioSnapshot& snapshot);
    /** Order used by snapshot (chain_symbol sort, then option symbol sort per chain). */
    [[nodiscard]] const std::vector<OptionData*>& option_apply_order() const {
        return option_apply_order_;
    }
    void set_underlying(const ContractData& contract);
    ChainData* get_chain(const std::string& chain_symbol);
    std::vector<std::string> get_chain_by_expiry(int min_dte, int max_dte) const;
    void add_option(const ContractData& contract);
    /** Sort chain indexes. */
    void finalize_chains();
    void calculate_atm_price();
};

} // namespace utilities
