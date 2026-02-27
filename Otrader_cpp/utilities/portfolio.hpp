#pragma once

/**
 * C++20 equivalent of utilities/portfolio.py
 * PortfolioData, ChainData, OptionData, UnderlyingData.
 */

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

// Minimal event engine interface (backtest implements; live EventEngine implements)
struct IEventEngine {
    virtual ~IEventEngine() = default;
    virtual void start() {}
    virtual void stop() {}
    /** Register handler for event type; default no-op, returns 0. */
    virtual uint64_t register_handler(EventType event_type,
                                      std::function<void(const Event&)> handler) {
        (void)event_type;
        (void)handler;
        return 0;
    }
    virtual void unregister_handler(EventType event_type, uint64_t token) {
        (void)event_type;
        (void)token;
    }
    /** Intent: send order (routed by event engine). Default returns empty. */
    virtual std::string put_intent_send_order(const OrderRequest& req) {
        (void)req;
        return {};
    }
    /** Intent: cancel order (routed by event engine). Default no-op. */
    virtual void put_intent_cancel_order(const CancelRequest& req) { (void)req; }
    /** Intent: log; EventEngine routes into LogEngine (sink stays inside LogEngine). Default no-op.
     */
    virtual void put_intent_log(const LogData& log) { (void)log; }
    /** Push event (ORDER/TRADE etc.) for dispatch; default no-op. */
    virtual void put_event(const Event& event) { (void)event; }
};

// Forward declarations
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
    int option_type = 1; // 1 = CALL, -1 = PUT
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
    std::unordered_set<std::string> index_set; // fast duplicate check during add_option
    double atm_price = 0;
    std::string atm_index;
    int days_to_expiry = 0;
    double time_to_expiry = 0;

    explicit ChainData(std::string chain_symbol);
    void add_option(OptionData* option);
    /** Sort indexes by numeric value (call once after all add_option). */
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
    /** Fixed order for compact snapshot apply (chain order then option order per chain). Built in
     * finalize_chains(). */
    std::vector<OptionData*> option_apply_order_;

    double risk_free_rate_ = 0.05;
    std::string iv_price_mode_ = "mid"; // "mid" | "bid" | "ask" for IV input price
    DateTime dte_ref_{};                // reference date for DTE (defaults to "now")

    explicit PortfolioData(std::string name);
    void set_risk_free_rate(double rate);
    void set_iv_price_mode(std::string mode);
    void set_dte_ref(DateTime ref);
    [[nodiscard]] DateTime dte_ref() const { return dte_ref_; }
    void update_option_chain(const ChainMarketData& market_data);
    void update_underlying_tick(const TickData& tick_data) const;
    /** Apply compact snapshot: compute IV/Greeks from snapshot (bid/ask/last + underlying), then
     * write to underlying + option_apply_order_. */
    void apply_frame(const PortfolioSnapshot& snapshot);
    /** Order used by snapshot (chain_symbol sort, then option symbol sort per chain). */
    [[nodiscard]] const std::vector<OptionData*>& option_apply_order() const {
        return option_apply_order_;
    }
    void set_underlying(const ContractData& contract);
    ChainData* get_chain(const std::string& chain_symbol);
    std::vector<std::string> get_chain_by_expiry(int min_dte, int max_dte) const;
    void add_option(const ContractData& contract);
    /** Sort each chain's indexes once (call after bulk add_option, e.g. after load). */
    void finalize_chains();
    void calculate_atm_price();
};

} // namespace utilities
