#pragma once

/** PortfolioData, ChainData, OptionData, UnderlyingData (from portfolio.py). */

#include "constant.hpp"
#include "event.hpp"
#include "object.hpp"
#include "symbol.hpp"
#include "utility.hpp"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <map>
#include <vector>

namespace model {

// Forward decls
struct PortfolioData;
struct ChainData;
struct UnderlyingData;

struct OptionData {
    std::string symbol;
    double size = 100.0;
    OptionData() = default;
    /** The contract's option, as its symbol names it (id). */
    OptionData(const ContractData& contract, const OptionId& id);
    double bid_price = 0.0;
    double ask_price = 0.0;
    double mid_price = 0.0;
    PortfolioData* portfolio = nullptr;

    double strike = 0.0;
    OptionType type = OptionType::CALL;
    DateTime expiry{}; // its close (option_expiry_time)
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
    double size = 1.0;
    double bid_price = 0.0;
    double ask_price = 0.0;
    double mid_price = 0.0;
    PortfolioData* portfolio = nullptr;
    double theo_delta = 1.0;
    std::unordered_map<std::string, ChainData*> chains;

    UnderlyingData() = default;
    explicit UnderlyingData(const ContractData& contract);
    void set_portfolio(PortfolioData* p);
    void add_chain(ChainData* chain);
};

struct ChainData {
    std::string chain_symbol;
    UnderlyingData* underlying = nullptr;
    std::unordered_map<std::string, OptionData*> options; // by symbol
    std::map<double, OptionData*> calls;                  // by strike
    std::map<double, OptionData*> puts;
    PortfolioData* portfolio = nullptr;
    /** Its strikes, ascending, each once (sort_strikes). */
    std::vector<double> strikes;
    /** The strike nearest reference_price() (the lower of two as near); 0: none. */
    double atm_strike = 0;
    int days_to_expiry = 0;
    double time_to_expiry = 0;
    /** The expiry's forward, from put-call parity on the last snapshot's quotes (spot grown at the
     * risk-free rate when no strike is quoted both ways); 0 before any, and once expired. */
    double forward = 0;
    /** A parity forward further than this from spot means bad quotes: grown spot instead. */
    static constexpr double kMaxForwardGap = 0.1;

    explicit ChainData(std::string chain_symbol);
    void add_option(OptionData* option);
    /** Count days_to_expiry / time_to_expiry from ref. */
    void count_days_to_expiry(DateTime ref);
    /** Sort the strikes, each once. */
    void sort_strikes();
    void set_underlying(UnderlyingData* u);
    void set_portfolio(PortfolioData* p);
    /** atm_strike from reference_price() (none quoted: the middle strike). */
    void calculate_atm_price();
    /** What the chain's moneyness is measured against: the forward, else the underlying's mid. */
    double reference_price() const;
    std::optional<double> get_atm_iv() const;
};

struct PortfolioData {
    std::string name;
    std::unordered_map<std::string, OptionData> options;
    std::unordered_map<std::string, std::unique_ptr<ChainData>> chains;
    std::unique_ptr<UnderlyingData> underlying;
    std::string underlying_symbol;
    /** What the portfolio hedges with when the underlying does not trade (an index; see
     * core/portfolios.hpp); empty: the underlying. */
    std::string hedge_symbol;
    /** Its options settle at expiry in shares of the underlying (SPY's), else in cash (SPXW's). */
    bool physical = false;
    /** Its data once its contract is loaded. Its theo_delta is in units of the underlying per unit
     * of it, at equal value: its price times its size over the underlying's. */
    std::unique_ptr<UnderlyingData> hedge_instrument;
    /** option_apply_order (finalize_chains). */
    std::vector<OptionData*> option_apply_order_;

    double risk_free_rate_ = 0.05;
    std::string iv_price_mode_ = "mid";
    DateTime dte_ref_{}; // what days to expiry count from; the epoch until the loader sets it
    DateTime datetime_{};

    explicit PortfolioData(std::string name);
    void set_risk_free_rate(double rate);
    void set_iv_price_mode(std::string mode);
    /** Count every chain's days to expiry from ref, a date (its UTC midnight): the loader sets
     * it, then each snapshot moves it to its own date. */
    void set_dte_ref(DateTime ref);
    [[nodiscard]] DateTime dte_ref() const { return dte_ref_; }
    /** Event time: when the quotes of the last applied snapshot were taken (the epoch before
     * any). Backtest: the bar's time; live: its portfolio's latest quote's. */
    [[nodiscard]] DateTime datetime() const { return datetime_; }
    /** Apply snapshot: event time, quotes, each chain's forward, then IV/Greeks as Black on that
     * forward (discounted at risk_free_rate_), into the underlying and option_apply_order. A
     * snapshot priced already (price_snapshot) is copied in; one that is not is priced here. */
    void apply_frame(const PortfolioSnapshot& snapshot);
    /** Price the snapshot: each chain's forward from its quotes by put-call parity (spot grown at
     * the risk-free rate when no strike is quoted both ways), then each option's IV and Greeks as
     * Black on that forward, into the snapshot (forward, expiry_years, iv, delta, gamma, theta,
     * vega; priced). Any thread: it reads only finalize_chains()' layout and the pricing
     * settings, which do not change while it runs (live, the market data thread prices; a
     * backtest prices its days before it runs them).
     *
     * quotes_changed (one per option of option_apply_order): the options whose quotes changed
     * since this snapshot was last priced. Only those, and those whose chain's forward or time to
     * expiry moved, are priced again; the rest keep their IV, their Greeks following the
     * underlying when it moved (underlying_changed): the same values as pricing all. Empty, or a
     * snapshot not priced before: all. */
    void price_snapshot(PortfolioSnapshot& snapshot, std::span<const std::uint8_t> quotes_changed = {},
                        bool underlying_changed = true) const;
    /** Order used by snapshot (chain_symbol sort, then option symbol sort per chain). */
    [[nodiscard]] const std::vector<OptionData*>& option_apply_order() const {
        return option_apply_order_;
    }
    void set_underlying(const ContractData& contract);
    void set_hedge_instrument(const ContractData& contract);
    /** The instrument hedges trade: the underlying, or hedge_symbol's when set (null until its
     * contract is loaded: nothing to hedge with). */
    [[nodiscard]] const UnderlyingData* hedge() const;
    ChainData* get_chain(const std::string& chain_symbol);
    std::vector<std::string> get_chain_by_expiry(int min_dte, int max_dte) const;
    void add_option(const ContractData& contract);
    /** Sort chain strikes; lay out option_apply_order and the pricing. */
    void finalize_chains();
    /** Lay out the pricing for option_apply_order as it is (finalize_chains does; a portfolio
     * built by hand calls it once its options are in order). Before anything prices. */
    void lay_out_pricing();
    void calculate_atm_price();

    // finalize_chains()' layout for price_snapshot, read-only after it. Per option of
    // option_apply_order: its strike (0: none), call or put, expiry (into pricing_expiries_; -1:
    // no option there) and chain (into pricing_chains_; -1: none). Per chain: its first option's
    // expiry (what its forward's time is counted to; -1: no option) and, in its calls' order, the
    // columns of the call and the put at each index that has both with a strike.
    std::vector<double> pricing_strike_;
    std::vector<std::uint8_t> pricing_is_call_;
    std::vector<int> pricing_expiry_;
    std::vector<int> pricing_chain_;
    std::vector<DateTime> pricing_expiries_;
    std::vector<ChainData*> pricing_chains_;
    std::vector<int> pricing_chain_expiry_;
    std::vector<std::vector<std::pair<int, int>>> pricing_pairs_;
    /** apply_frame's copy of a snapshot that came unpriced, priced here (event thread). */
    PortfolioSnapshot unpriced_;

  private:
    /** The chain's forward for the snapshot (its expiry_years set): by put-call parity, the median
     * over the (up to) three strikes nearest it, quoted both ways; spot grown at the risk-free
     * rate when none is, or when it is further than kMaxForwardGap from spot. */
    double chain_forward(std::size_t chain, const PortfolioSnapshot& snapshot, double spot) const;
};

} // namespace model
