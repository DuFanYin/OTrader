#pragma once

/**
 * The offline data research reads (data/INVENTORY.md §2.3, §2.4), a contract or a row group at a
 * time.
 *
 * NBBO ticks, as data/scripts/convert.py ticks stores them: a file per day and expiry, a
 * contract's rows together, a price an integer at the file's places (metadata price_places), null
 * where it is the row before's in the same contract (never on a contract's or a row group's first
 * row). Read a row group at a time and handed out a contract at a time, each as soon as its rows
 * are all read: one contract is in memory, however big the file.
 *
 * Trades, as data/scripts/convert.py trades stores them: a file a day, strike in thousandths, the
 * trade's time in milliseconds into the New York day, its price an integer at the file's places;
 * read by contract (read_trades) or as a day's columns (read_trade_rows).
 *
 * A backtest day's open interest: one value a contract a day (read_open_interest).
 *
 * Every file is a portfolio's: .../<PORTFOLIO>/<PORTFOLIO>-YYYY-MM/YYYYMMDD[...].parquet; one
 * named for a day in its portfolio's holdout is refused (Holdouts), and the screens of backtest
 * days (structures, gamma) refuse a day whose quotes are in it, whatever its name.
 */

#include <chrono>
#include <compare>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace arrow {
class Array;
class Table;
} // namespace arrow

namespace research {

struct Contract {
    double strike = 0.0;
    bool call = false;
    auto operator<=>(const Contract&) const = default;
};

/** The NBBO at each change: milliseconds since the epoch, bid and ask in dollars. */
struct QuoteSeries {
    std::vector<std::int64_t> ms;
    std::vector<double> bid;
    std::vector<double> ask;
};

struct TradeSeries {
    std::vector<std::int64_t> ms;
    std::vector<double> price;
};

/** A day's trades, a column each: price, bid and ask in dollars (NaN where null), the strike in
 * dollars, the time in milliseconds into the New York day. */
struct TradeRows {
    std::vector<int> expiry; // YYYYMMDD
    std::vector<double> strike;
    std::vector<std::int8_t> call;
    std::vector<std::int32_t> ms;
    std::vector<std::int16_t> condition;
    std::vector<std::int16_t> exchange;
    std::vector<double> price, bid, ask;
    std::vector<std::int32_t> size;
    [[nodiscard]] std::size_t rows() const { return ms.size(); }
};

/** Calls visit with the ticks of each contract whose strike keep accepts, in time order. */
std::expected<void, std::string>
for_each_contract(const std::string& path, const std::function<bool(double strike)>& keep,
                  const std::function<void(const Contract&, QuoteSeries&&)>& visit);

/** The trades of one expiry (YYYYMMDD) by contract, in time order; ms since the epoch, given the
 *  day's New York midnight in ms since the epoch. */
std::expected<std::map<Contract, TradeSeries>, std::string>
read_trades(const std::string& path, int expiry, std::int64_t day_start_ms);

/** A backtest day's open interest by contract (its OCC code without the root, model::Contract's
 * name): the day's, one value a contract (none: no value). */
std::expected<std::unordered_map<std::string, double>, std::string>
read_open_interest(const std::string& path);

/** A contract's single-leg trades of a day, each signed by the NBBO before it (FlowTable writes
 * them, measure_gamma sums them into the customers' positions): days and expiries YYYYMMDD, the
 * strike in thousandths. */
struct ContractNet {
    std::int32_t day = 0, expiry = 0, strike_milli = 0;
    bool call = false;
    std::int32_t net = 0; // bought less sold, contracts
    std::int32_t volume = 0;
};
/** A contract in one integer: its expiry YYYYMMDD (25 bits), strike in thousandths (24), call. */
constexpr auto contract_key(std::int32_t expiry, std::int32_t strike_milli, bool call)
    -> std::uint64_t {
    return (static_cast<std::uint64_t>(expiry) << 25) |
           (static_cast<std::uint64_t>(strike_milli) << 1) | static_cast<std::uint64_t>(call);
}
/** The expiry (YYYYMMDD) of a contract_key. */
constexpr auto key_expiry(std::uint64_t key) -> std::int32_t {
    return static_cast<std::int32_t>(key >> 25);
}
std::expected<std::vector<ContractNet>, std::string> read_contract_nets(const std::string& path);

/** A day's trades of these conditions (all when none), read a row group at a time. */
std::expected<TradeRows, std::string>
read_trade_rows(const std::string& path, std::span<const std::int16_t> conditions = {});

/** A Parquet column of the values (built in offline_data.cpp: arrow's headers stay out of the
 * users'). */
std::shared_ptr<arrow::Array> column(const std::vector<std::int8_t>& values);
std::shared_ptr<arrow::Array> column(const std::vector<int>& values);
std::shared_ptr<arrow::Array> column(const std::vector<std::int64_t>& values);
std::shared_ptr<arrow::Array> column(const std::vector<double>& values);
std::shared_ptr<arrow::Array> column(const std::vector<bool>& values);
std::shared_ptr<arrow::Array> column(const std::vector<std::string>& values);
/** A column of a list a row. */
std::shared_ptr<arrow::Array> list_column(const std::vector<std::vector<double>>& values);
std::shared_ptr<arrow::Array> list_column(const std::vector<std::vector<int>>& values);
std::shared_ptr<arrow::Array> list_column(const std::vector<std::vector<bool>>& values);
/** The table written to path as Parquet, a million rows a row group. */
std::expected<void, std::string> write_table(const std::string& path,
                                             const std::shared_ptr<arrow::Table>& table);

/** YYYYMMDD of a date. */
constexpr auto yyyymmdd(std::chrono::year_month_day d) -> int {
    return (static_cast<int>(d.year()) * 10000) + (static_cast<int>(unsigned{d.month()}) * 100) +
           static_cast<int>(unsigned{d.day()});
}
/** A date written YYYY-MM-DD; throws std::invalid_argument on anything else. */
std::chrono::year_month_day parse_day(const std::string& text);

/** Each portfolio's first holdout day, YYYYMMDD, from $OPTRA_HOME/config.json (default
 * ~/.optra/config.json: {"holdout": {"SPXW": "2025-07-01"}}), read as the server reads it
 * (ledger.holdout_from): a portfolio with none of its own while others have one takes the earliest
 * of theirs. */
class Holdouts {
  public:
    static Holdouts load();
    /** The portfolio's first holdout day, YYYYMMDD; 0 for none. */
    [[nodiscard]] int of(std::string_view portfolio) const;
    /** The first of the files whose day by its name is in its portfolio's holdout (the server goes
     * by the quotes' own times first: a file renamed passes here, not there). */
    [[nodiscard]] std::optional<std::string> first_held(std::span<const std::string> files) const;

  private:
    std::map<std::string, int, std::less<>> by_portfolio_;
    int earliest_ = 0;
};

} // namespace research
