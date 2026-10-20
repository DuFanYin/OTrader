#pragma once

/**
 * Resting limit orders on one contract's quotes and trades: placed at t0 at the mid plus e half
 * spreads toward their side, did they fill within a window, and what did they cost.
 *
 * Two fill rules: touch (the opposite quote reaches the limit or a trade prints at it: as if first
 * in the queue) and through (beyond it: any queue ahead is certainly gone). Not filled by the
 * window's end, an order pays the opposite quote then. Its cost is the price paid less the mid at
 * the window's end (the same mark filled or not, so what tends to follow a fill, the adverse
 * selection, is in it), in half spreads at t0, positive a cost. A market order at t0 costs 1 plus
 * the move to the window's end (market_cost). FillTable writes them, a row an order, as Parquet.
 *
 * Where trades print in the NBBO just before them (place_trades, place_combos): e = |price - mid|
 * / half spread, 0 at the mid, 1 at the bid or ask. Billions of trades: summed into TradeCells,
 * whose bins the caller chooses. And how an option's NBBO moves within a minute against the
 * minute quotes a backtest sees (sample_intraminute), a row a contract and minute.
 */

#include "research/offline_data.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace research {

struct LimitOrders {
    std::span<const std::int64_t> t0_ms;     // when each order reaches the market
    std::span<const std::int64_t> window_ms; // how long each rests
    std::span<const double> fill_positions;  // e: the limit at mid + e half spreads
    double tick_size = 0.01;                 // limits rounded to it, away from the other side
};

struct LimitFill {
    std::int64_t t0_ms = 0;
    std::int64_t window_ms = 0;
    int side = 1; // 1 buy, -1 sell
    double fill_position = 0.0;
    double mid0 = 0.0;
    double spread0 = 0.0;
    double market_cost = 0.0;
    bool filled_touch = false;
    bool filled_through = false;
    double cost_touch = 0.0;
    double cost_through = 0.0;
};

/** Every order (each t0, window, e, both sides) whose quotes at t0 and at the window's end are
 *  two-sided. */
std::vector<LimitFill> simulate_limits(const QuoteSeries& quotes, const TradeSeries& trades,
                                       const LimitOrders& orders);

class FillTable {
  public:
    /** The orders of one contract on one day (YYYYMMDD), days to its expiry. */
    void add(const Contract& contract, int day, int dte, const std::vector<LimitFill>& fills);
    std::size_t rows() const { return t0_ms_.size(); }
    std::expected<void, std::string> write(const std::string& path) const;

  private:
    std::vector<int> day_, dte_;
    std::vector<double> strike_;
    std::vector<std::int8_t> call_, side_;
    std::vector<std::int64_t> t0_ms_, window_ms_;
    std::vector<double> e_, mid0_, spread0_, market_cost_, cost_touch_, cost_through_;
    std::vector<bool> filled_touch_, filled_through_;
};

/** A key of TradeCells: a trade's measure in bins at edges, left closed (as polars' cut), a label
 * each (edges + 1); no edges: the measure's own labels (one that has them), else its integer value.
 */
struct CellKey {
    std::string name;
    std::string measure; // empty: the name's
    std::vector<double> edges;
    std::vector<std::string> labels;
};

/** Trades summed by cell, a bin of each key (up to 8, of up to 254 bins); a trade whose measure is
 * NaN falls in its key's null bin, a NaN value adds nothing (as polars' null). */
class TradeCells {
  public:
    struct Measure {
        std::string_view name;
        std::vector<std::string> labels; // a measure that is an index into them
    };
    /** Throws std::invalid_argument for a key on a measure there is none of, or bins not labelled.
     */
    TradeCells(std::vector<CellKey> keys, std::vector<Measure> measures,
               std::vector<std::string_view> sums);
    /** The trade's measures, in the order given; its values, one a sum. */
    void add(std::span<const double> measures, std::span<const double> values);
    void merge(const TradeCells& other);
    [[nodiscard]] std::size_t cells() const { return sums_of_.size(); }
    /** A row a cell: its keys' labels (strings, null in a null bin), then its sums. */
    std::expected<void, std::string> write(const std::string& path) const;

  private:
    static constexpr std::uint8_t kNull = 255;
    struct Key {
        std::size_t measure = 0;
        std::vector<double> edges;
        std::vector<std::string> labels;
        bool values = false; // labelled by the measure's integer values, as they come
        std::unordered_map<std::int64_t, std::uint8_t> bin_of_value;
    };
    std::uint8_t bin(Key& key, double v);

    std::vector<std::string> names_;
    std::vector<Key> keys_;
    std::vector<std::string_view> sums_;
    std::unordered_map<std::uint64_t, std::vector<double>>
        sums_of_; // by the keys' bins, a byte each
};

/** place_trades' measures and sums, in order; kind by condition (the vendor's codes). */
TradeCells trade_cells(std::vector<CellKey> keys);
/** Each trade of a day into cells: measures year, kind, dte, otm (% out of the money, from the
 * underlying's mid the minute before; minutes since New York midnight, sorted), size, half (the
 * half spread), mid, ms (into the day); sums trades, contracts, e_sum, e_size_sum, at_mid,
 * inside, at_quote, outside, half_sum, saved_usd_per_share_sum (those of e over trades with a
 * two-sided quote). */
void place_trades(const TradeRows& trades, std::chrono::year_month_day day,
                  std::span<const int> minutes, std::span<const double> underlying,
                  TradeCells& cells);

/** The conditions of complex (multi-leg) trades. */
inline constexpr std::array<std::int16_t, 14> kComboConditions{130, 131, 132, 133, 134, 135, 136,
                                                               137, 138, 139, 140, 141, 142, 143};
/** place_combos' measures and sums, in order. */
TradeCells combo_cells(std::vector<CellKey> keys);
/** A day's complex trades (those of kComboConditions) into cells. A trade is its legs of one
 * millisecond, exchange and condition, quoted two-sided, told by its contracts and their sizes:
 * of one expiry a vertical (one right, 1:1), a straddle (a put and a call, one strike, 1:1), a
 * strangle (the put's strike below the call's), an iron condor or butterfly (2 puts at or below 2
 * calls, 1:1:1:1, inner bought), a butterfly (one right, 1:2:1); of two a calendar (one right, one
 * strike, 1:1; its dte the near leg's); others left out. Per unit:
 * e = |net - its mid| / what legging in at the bid and ask costs. Measures year, structure, dte,
 * units, legged, ms, condition; sums trades, units, e_sum, e_units_sum, at_mid, within_half,
 * legged_or_worse, legged_usd_sum, saved_usd_sum. */
void place_combos(const TradeRows& trades, std::chrono::year_month_day day, TradeCells& cells);

/** The order flow: each trade signed by where it prints in the NBBO before it (above the mid +1,
 * the buyer took the ask's side; below -1; at the mid or unquoted 0), the taker taken for the
 * customer and the other side for a dealer. Its Greeks per contract from Black on the underlying's
 * mid the minute before (forward = spot, no rate), at the vol of the NBBO's mid; to expiries 45
 * days or nearer, the time to 16:00 of the expiry at least 5 minutes, else none (0 in the sums).
 * Summed a cell a day: bin (15 minutes from 09:30, 0..25; -1 before, 26 from 16:00), dte (0, 1,
 * 2: 2-7, 3: 8-45, 4: further), call, multi (the multi-leg and floor conditions), size (0: 1-9,
 * 1: 10-99, 2: 100+); sums trades, contracts, buy, sell (contracts), delta_shares (signed, x100),
 * abs_delta_shares, gamma_usd (signed: the dollar delta a 1% move adds, Γ S² / 100 x100),
 * vega_usd (signed, per 1% vol, x100), premium_usd (signed). And single-leg trades of at least
 * big_size contracts, a row each; and each contract's single-leg trades of the day, a row a
 * contract (write_contracts): net (bought less sold, contracts) and volume, what measure_gamma
 * sums into the customers' positions. */
class FlowTable {
  public:
    static constexpr std::size_t kSums = 9;
    struct Cell {
        std::int32_t day = 0; // days since the epoch
        std::int8_t bin = 0, dte = 0;
        bool call = false, multi = false;
        std::int8_t size = 0;
        std::array<double, kSums> sums{};
    };
    struct BigTrade {
        std::int32_t day = 0, ms = 0, size = 0;
        std::int16_t dte = 0, condition = 0, exchange = 0;
        bool call = false;
        std::int8_t sign = 0;
        double strike = 0, price = 0, bid = 0, ask = 0, spot = 0, iv = 0, delta = 0, gamma = 0;
    };

    explicit FlowTable(int big_size) : big_size_(big_size) {}
    /** minutes since New York midnight (sorted) and the underlying's mid at each. */
    void add(const TradeRows& trades, std::chrono::year_month_day day,
             std::span<const int> minutes, std::span<const double> underlying);
    void merge(FlowTable&& other);
    [[nodiscard]] std::size_t rows() const { return cells_.size(); }
    [[nodiscard]] std::size_t big_rows() const { return big_.size(); }
    std::expected<void, std::string> write(const std::string& path) const;
    std::expected<void, std::string> write_big(const std::string& path) const;
    std::expected<void, std::string> write_contracts(const std::string& path) const;

  private:
    int big_size_;
    std::vector<Cell> cells_;
    std::vector<BigTrade> big_;
    std::vector<ContractNet> contracts_;
};

/** One contract's NBBO at each minute m (milliseconds since the epoch) where it is two-sided at
 * m, m + 1 s (an order decided at m reaching the market), a random time in the minute (from rng)
 * and m + 60 s (the next minute's quote, where a backtest fills an order sent at m). */
struct IntraminuteRows {
    std::vector<std::int16_t> minute; // its index in the minutes given
    std::vector<float> mid_m, spread_m, spread_r, ask_d, bid_d, ask_n, bid_n, mid_d, mid_r, mid_n;
};
void sample_intraminute(const QuoteSeries& quotes, std::span<const std::int64_t> minutes,
                        std::uint64_t seed, IntraminuteRows& out);
/** Rows of many days into one file: day (a date), dte, then IntraminuteRows' columns. */
std::expected<void, std::string> write_intraminute(
    const std::string& path, const std::vector<std::pair<std::chrono::year_month_day, int>>& days,
    const std::vector<IntraminuteRows>& rows);

} // namespace research
