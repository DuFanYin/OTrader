#pragma once

/**
 * Research on the backtest days, through the engine's own day reading, pricing and leg picking
 * (adapters::HistoricalData, adapters::Replay, model::leg_option): what it sees is what a backtest
 * of the same rule sees.
 * - Option structures screened: a structure (its legs model::LegRules) decided at chosen minutes of
 *   every day and valued at the next bar's quotes, as a backtest fills an order, then at chosen
 *   exits. Nothing is ordered or held; each entry is valued on its own. StructureTable has a row
 *   per entry and exit, amounts in USD a unit (each leg's contracts times its multiplier).
 * - The dealers' gamma by the day's open interest (measure_gamma, GammaTable).
 * - The underlying at chosen minutes (read_underlying).
 */

#include "adapters/market_data/historical_data.hpp"
#include "model/portfolio.hpp"

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

namespace research {

struct StructureExit {
    enum class At : std::uint8_t {
        kExpiry,    // the first leg's expiry: its legs at their intrinsic value, others quoted
        kDaysAfter, // days trading days after the entry (0: the same day), at minute
        kDaysBeforeExpiry, // days trading days before the first leg's expiry, at minute
    };
    std::string label;
    At at = At::kExpiry;
    int days = 0;
    int minute = 389; // since the open
};

/** A leg: its rule, or another leg's expiry or strike (a calendar's far leg: the near one's
 * strike on its own expiry). */
struct StructureLeg {
    model::LegRule rule;
    int expiry_of = -1; // a leg before it whose chain it takes; -1: its rule's
    int strike_of = -1; // a leg before it whose strike it takes; -1: its rule's
};

struct Structure {
    std::string name;
    std::vector<StructureLeg> legs;
    std::vector<int> entry_minutes;
    std::vector<StructureExit> exits;
};

struct ScreenSpec {
    std::vector<std::string> days; // one portfolio's day files, in date order
    int holdout = 0; // YYYYMMDD: a day whose quotes are from it on is refused (Holdouts); 0: none
    std::string cache_dir;
    bool store_chains = false;   // the chains read here stored in the cache (its day indexes are)
    std::size_t days_ahead = 4;  // days loaded ahead of their turn, each on a thread of its own
    double risk_free_rate = adapters::HistoricalData::kRiskFreeRate;
    std::string iv_price_mode{adapters::HistoricalData::kIvPriceMode};
    std::vector<Structure> structures;
};

class StructureTable {
  public:
    /** An entry's rows, one per exit: the strikes, expiries (YYYYMMDD), ratios and rights of its
     * legs; at entry the underlying, the first leg's forward and implied move (its ATM straddle's
     * mid over the forward), the mid, the cost at the quotes (paid positive), delta and vega; at
     * each exit its day and minute (when due, if not reached), the mid, what closing at the quotes
     * takes in and the underlying (NaN when not reached: no quote, no day), and whether its legs
     * were settled. */
    struct Row {
        int structure = 0;
        int entry_day = 0;
        int entry_minute = 0;
        int exit = 0;
        int exit_day = 0;
        int exit_minute = 0;
        bool settled = false;
        double underlying0 = 0.0, forward0 = 0.0, move0 = 0.0;
        double entry_mid = 0.0, entry_quotes = 0.0, entry_delta = 0.0, entry_vega = 0.0;
        double exit_mid = 0.0, exit_quotes = 0.0, underlying1 = 0.0;
    };

    explicit StructureTable(const std::vector<Structure>& structures);
    void add(const Row& row, const std::vector<double>& strikes, const std::vector<int>& expiries,
             const std::vector<int>& ratios, const std::vector<bool>& calls);
    [[nodiscard]] std::size_t rows() const { return structure_.size(); }
    std::expected<void, std::string> write(const std::string& path) const;

  private:
    const std::vector<Structure>& structures_;
    std::vector<int> structure_, entry_day_, entry_minute_, exit_day_, exit_minute_;
    std::vector<std::string> name_, exit_;
    std::vector<bool> settled_;
    std::vector<double> underlying0_, forward0_, move0_, entry_mid_, entry_quotes_, entry_delta_,
        entry_vega_, exit_mid_, exit_quotes_, underlying1_;
    std::vector<std::vector<double>> strikes_;
    std::vector<std::vector<int>> expiries_, ratios_;
    std::vector<std::vector<bool>> calls_;
};

/** The structures run over the days into table; the days that could not be read, each with why
 * (a run goes on past them: an exit falling on one is not reached). */
std::expected<std::vector<std::string>, std::string> screen_structures(const ScreenSpec& spec,
                                                                       StructureTable& table);

/** What measure_gamma measures: the days (one portfolio's), at these minutes since the open (the
 * bar of each), the chains up to max_dte calendar days out. */
struct GammaSpec {
    std::vector<std::string> days;
    int holdout = 0; // YYYYMMDD: a day whose quotes are from it on is refused (Holdouts); 0: none
    std::string cache_dir;
    double risk_free_rate = adapters::HistoricalData::kRiskFreeRate;
    std::string iv_price_mode{adapters::HistoricalData::kIvPriceMode};
    std::vector<int> minutes{15};
    int max_dte = 45;
    std::size_t days_ahead = 4; // days measured at once, each on a thread of its own
    /** Each contract's trades a day (FlowTable::write_contracts); none: no gex_flow. */
    std::string contract_nets;
};

/** The dealers' gamma by the day's open interest (the convention: they hold the calls customers
 * sold them and are short the puts customers bought), a row a day and minute: the underlying's
 * mid; net gamma in USD a 1% move (sum of sign x open interest x a contract's gamma x spot^2 x
 * 0.01, calls +, puts -), of all chains measured and of the day's expiry, the calls' and puts'
 * parts; the calls' and puts' open interest; the strike with the most open-interest gamma within
 * 2% of the spot (0: none). With contract_nets, also the dealers' gamma by the customers' positions
 * instead of the convention: a contract's position the sum of its nets on the days before (what
 * customers bought less sold), the dealers' gamma minus its gamma (gex_flow, gex_flow_0dte), and
 * the positions' absolute sum (flow_contracts, against the open interest). */
class GammaTable {
  public:
    struct Row {
        int day = 0;
        int minute = 0;
        double spot = 0.0;
        double gex = 0.0, gex_0dte = 0.0, gex_calls = 0.0, gex_puts = 0.0;
        double oi_calls = 0.0, oi_puts = 0.0;
        double peak_strike = 0.0;
        double gex_flow = 0.0, gex_flow_0dte = 0.0, flow_contracts = 0.0;
    };
    void add(const Row& row) { rows_.push_back(row); }
    [[nodiscard]] std::size_t rows() const { return rows_.size(); }
    std::expected<void, std::string> write(const std::string& path) const;

  private:
    std::vector<Row> rows_;
};

/** The days measured into table, in date order; the days that could not be, each with why. */
std::expected<std::vector<std::string>, std::string> measure_gamma(const GammaSpec& spec,
                                                                   GammaTable& table);

/** The underlying's bid and ask at these minutes since the open (the bar of each) of each day, read
 * from the days' indexes (the cache's): a row a day and minute, the day YYYYMMDD; the days that
 * could not be read, each with why. */
struct UnderlyingRow {
    int day = 0;
    int minute = 0;
    double bid = 0.0;
    double ask = 0.0;
};
std::expected<std::vector<std::string>, std::string>
read_underlying(const std::vector<std::string>& days, const std::vector<int>& minutes,
                const std::string& cache_dir, std::vector<UnderlyingRow>& rows);
std::expected<void, std::string> write_underlying(const std::string& path,
                                                  const std::vector<UnderlyingRow>& rows);

} // namespace research
