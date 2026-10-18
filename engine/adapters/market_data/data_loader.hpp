#pragma once

/**
 * A day's data in files, read as numbers: the day file (data/<ROOT>/<ROOT>-YYYY-MM/<day>.parquet)
 * as the conversion writes it, and the day as the backtest cache stores it (data/cache/days, a
 * file for the day and one for each chain loaded). What a day makes of the numbers is
 * HistoricalData's.
 */

#include "model/object.hpp"
#include "model/portfolio.hpp"
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace adapters {

// ---- What the files hold

/**
 * A day file (data/<ROOT>/<ROOT>-YYYY-MM/<day>.parquet) as it is: its rows in time order (as
 * written within a time), each an option's quote at a time with the underlying's at that time;
 * and the rows of each time. A null price or size is 0; a null underlying price is NaN (none at
 * that row).
 */
struct ParquetQuotes {
    struct Minute {
        model::DateTime time{};
        std::size_t first = 0; // its rows: [first, end)
        std::size_t end = 0;
    };

    std::string path;
    std::vector<std::string> codes; // the options' codes (OCC without the root), each once, sorted
    std::vector<Minute> minutes;    // in time order
    // Per row:
    std::vector<std::uint32_t> code; // into codes
    std::vector<double> bid;
    std::vector<double> ask;
    std::vector<double> bid_size;
    std::vector<double> ask_size;
    std::vector<double> underlying_bid;
    std::vector<double> underlying_ask;

    [[nodiscard]] std::size_t rows() const { return code.size(); }
};

/** A day file read whole (columns ts_recv, symbol; bid_px, ask_px, bid_sz, ask_sz,
 * underlying_bid_px, underlying_ask_px when it has them; no others; a price a double or a decimal);
 * why not when it cannot be read or a column has another type. A relative path is from the repository (the working directory, or
 * its parent from build/). */
std::expected<ParquetQuotes, std::string> read_parquet_quotes(const std::string& path);

/** The trading day a file holds, from its name (data/SPY/SPY-2025-03/20250303.parquet); none when
 * the name is not a date. */
std::optional<std::chrono::year_month_day> day_of_file(const std::string& file);

/** The portfolio a file is of: its folder's (data/<PORTFOLIO>/<PORTFOLIO>-YYYY-MM/<day>), or
 * the name's (backtest_<PORTFOLIO>_<start>_<end>.parquet). */
std::string portfolio_of_file(const std::string& file);

/** The portfolios the files are of, each once. */
std::vector<std::string> portfolios_of_files(std::span<const std::string> files);

/** A portfolio's contracts from its options' codes (OCC without the root), in their order: its
 * underlying, then each option; a code that is not one is left out. */
std::vector<model::Contract> contracts_of_codes(const std::string& portfolio,
                                                std::span<const std::string> codes);

/** A portfolio's contracts as a broker's sync would give them: its underlying and every option the
 * files of that portfolio quote. Throws when a file cannot be read. */
std::vector<model::Contract> contracts_of_files(std::span<const std::string> files,
                                                std::string_view portfolio);

/** The first and the last time the files quote: what a replay of them runs over. Throws when a
 * file cannot be read or quotes nothing. */
std::pair<model::DateTime, model::DateTime> time_range_of_files(std::span<const std::string> files);

// ---- A day for HistoricalData

/** A day's contracts, minutes and underlying: what a day is before any chain is loaded. */
struct DayIndex {
    std::string portfolio;               // the day's options' root, from the path
    std::optional<model::DateTime> day;  // its first minute's day, at 00:00 UTC
    std::int64_t rows = 0;               // the file's
    model::Contract underlying;
    std::shared_ptr<const model::Listing> listing; // its options
    // Per minute, in time order: its time (model::DateTime ticks) and the underlying's last quote
    // in it (0: none).
    std::vector<std::int64_t> times;
    std::vector<double> underlying_bid;
    std::vector<double> underlying_ask;
    // The risk-free curve the file carries (its key-value metadata risk_free_curve); none: none.
    std::optional<model::RateCurve> rates;
};

/** The day file's index (the columns ts_recv, symbol, underlying_bid_px, underlying_ask_px only,
 * and the curve its metadata carries); why not when it cannot be read. */
std::expected<DayIndex, std::string> read_day_index(const std::string& path);

/**
 * A chain's minutes as the cache stores them and a day reads them, in place: per column (bid, ask,
 * bid_size, ask_size, iv: model::PortfolioSnapshot::kOptionColumns' first five) a row of its
 * options (in the listing's order) a minute, then its forward a minute, all doubles. Its bytes are
 * the file's: mapped from the cache, or made here to fill and store. Copies share them.
 */
class StoredChain {
  public:
    static constexpr std::size_t kColumns = 5;

    /** Zeros for these options over `minutes`, to fill. */
    StoredChain(std::string_view chain, std::span<const model::Contract> options,
                std::size_t minutes);
    /** A stored chain's file mapped, when it is this chain's with these options and this many
     * minutes. Reading sets its access time: the cache lets go of the files read longest ago
     * first. */
    static std::optional<StoredChain> read(const std::string& file, std::string_view chain,
                                           std::span<const model::Contract> options,
                                           std::size_t minutes);
    /** Written whole (through a file renamed into place); false (said on stderr) when it cannot
     * be. */
    bool store(const std::string& file) const;

    [[nodiscard]] std::size_t options() const { return options_; }
    /** Column `column`'s values at minute i, an option each. */
    [[nodiscard]] std::span<const double> row(std::size_t column, std::size_t i) const {
        return {values(column) + (i * options_), options_};
    }
    /** To fill: of one made here only (a mapped one is read only). */
    [[nodiscard]] std::span<double> row(std::size_t column, std::size_t i) {
        return {const_cast<double*>(values(column)) + (i * options_), options_};
    }
    [[nodiscard]] double forward(std::size_t i) const { return values(kColumns)[i]; }
    void set_forward(std::size_t i, double value) {
        const_cast<double*>(values(kColumns))[i] = value;
    }

  private:
    StoredChain() = default;
    /** Where column `column` starts (kColumns: the forward). */
    [[nodiscard]] const double* values(std::size_t column) const {
        return reinterpret_cast<const double*>(bytes_.get() + runs_[column]);
    }

    std::shared_ptr<std::byte> bytes_; // the file's
    std::size_t size_ = 0;
    std::size_t options_ = 0;
    std::array<std::size_t, kColumns + 1> runs_{}; // where each column starts, then the forward
};

/** The quotes (bid, ask and their sizes) of a chain's options (all of one expiry, `name` their
 * OCC code without the root) from the day file into `stored`: each minute an option's last row in
 * it, else the minute before's. The row groups whose symbols are all of other expiries are not
 * read; a row at a time not among the day's times is left out. Why not when the file cannot be
 * read. */
std::expected<void, std::string> read_chain_quotes(const std::string& path,
                                                   std::span<const std::int64_t> times,
                                                   std::span<const model::Contract> options,
                                                   StoredChain& stored);

/** Where cache_dir keeps a day: <cache_dir>/<ROOT>/<YYYYMMDD>-<key>.day, keyed by the file's name,
 * size and time written and the format. Not by the file's place or other code: a change elsewhere
 * keeps it. */
std::string stored_day(const std::string& cache_dir, const std::string& parquet_path);
/** And one of its chains: <YYYYMMDD>-<chain>-<key>.chain beside it, keyed also by the format and
 * what it was priced with: the pricing code's fingerprint (pricing_fingerprint), the risk-free
 * rate and the IV's price. */
std::string stored_chain(const std::string& cache_dir, const std::string& parquet_path,
                         std::string_view chain, std::uint64_t pricing, double risk_free_rate,
                         std::string_view iv_price_mode);
/** The day's file is in cache_dir. */
bool day_stored(const std::string& cache_dir, const std::string& parquet_path);

/** A stored day's file (stored_day) read back, bit for bit; none when it is not one. Reading sets
 * its access time: the cache lets go of the files read longest ago first. */
std::optional<DayIndex> read_stored_day(const std::string& file);
/** Written whole (through a file renamed into place); false (said on stderr) when it cannot be. */
bool store_day(const std::string& file, const DayIndex& day);

} // namespace adapters
