#pragma once

/**
 * HistoricalData: a backtest day in memory, its chains loaded and priced as they are wanted; read
 * from its files (and stored in a cache directory) through data_loader.
 */

#include "adapters/market_data/data_loader.hpp"
#include "model/object.hpp"
#include "model/portfolio.hpp"
#include "model/portfolios.hpp"
#include "utilities/log_source.hpp"
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

// ---- HistoricalData

/**
 * One backtest day: its underlying, its options' listing and per minute the underlying's quotes
 * (load); each chain's quotes and their pricing once the chain is loaded (load_chains), as the
 * engines' subscriptions and holdings want them, the way live streams only what it is asked for.
 * A chain's minutes are one block (StoredChain), read where the cache keeps them; a chain not
 * loaded has none, and reads as zeros. Read only once a chain is loaded, so many engines read it at
 * once, and any of their threads may load chains: each is loaded once. Of the pricing it keeps what
 * solving for the IV gives (each chain's forward, each expiry's time, each option's IV); an
 * option's Greeks are priced from it when asked (model::greeks_at), bit for bit what pricing the
 * whole day gives. Each expiry is priced with the risk-free rate to it on the curve the day's file
 * carries (its risk_free_curve), else at the one rate set. Stored (with a cache_dir) as a file for
 * the day and one for each chain loaded, read back to the same values bit for bit.
 */
class HistoricalData {
  public:
    /** The pricing settings a day has until set otherwise. */
    static constexpr double kRiskFreeRate = 0.05;
    static constexpr std::string_view kIvPriceMode = "mid";

    explicit HistoricalData(utilities::Logger* logger = nullptr);
    ~HistoricalData();
    HistoricalData(const HistoricalData&) = delete;
    HistoricalData& operator=(const HistoricalData&) = delete;

    /** Before load: what its chains are priced with; the rate at every maturity when the day's file
     * carries no curve. */
    void set_risk_free_rate(double rate);
    void set_iv_price_mode(std::string_view mode);
    [[nodiscard]] double risk_free_rate() const { return risk_free_rate_; }
    /** The day's rates: the file's curve, else the rate set at every maturity. */
    [[nodiscard]] const model::RateCurve& rates() const { return rates_; }
    [[nodiscard]] std::string_view iv_price_mode() const { return iv_price_mode_; }

    /** The day's contracts, minutes and underlying, no chain loaded: from cache_dir when stored
     * there (stored_day), else read from the file and stored there; no cache_dir: neither. Its
     * portfolio is the path's (data/<ROOT>/..). */
    void load(const std::string& parquet_path, const std::string& cache_dir = {});
    /** These chains (model::chain_symbol) loaded and priced, each once, from the cache_dir the
     * day was loaded with when stored there (stored_chain), else read from the file and stored;
     * any thread. A chain the day does not list is left out; one that cannot be read is zeros,
     * why in load_error. */
    void load_chains(std::span<const std::string> chains);
    /** Every chain the day lists. */
    void load_all_chains();
    /** The chains loaded so far, in the listing's order. */
    [[nodiscard]] std::vector<std::string> loaded_chains() const;

    [[nodiscard]] bool has_data() const { return loaded_; }
    /** Why the last load failed, or a chain could not be read (empty: neither). */
    [[nodiscard]] std::string load_error() const;
    /** The file it was read from, and its rows. */
    [[nodiscard]] const std::string& path() const { return path_; }
    [[nodiscard]] std::int64_t rows() const { return index_.rows; }
    [[nodiscard]] const std::string& portfolio_name() const { return index_.portfolio; }

    [[nodiscard]] const model::Contract& underlying() const { return index_.underlying; }
    /** Its options, in the order of its frames' columns. */
    [[nodiscard]] const std::shared_ptr<const model::Listing>& listing() const {
        return index_.listing;
    }
    /** The day, at 00:00 UTC: what its days to expiry count from; {} when none is loaded. */
    [[nodiscard]] model::DateTime day() const { return index_.day.value_or(model::DateTime{}); }
    /** The day's portfolio in `target`, set up as live sets it up (model::kPortfolios), listing
     * the day's options: for an engine's first day (backtest::Replay::roll). */
    void build_portfolio(model::Portfolios& target) const;

    [[nodiscard]] std::size_t minutes() const { return index_.times.size(); }
    /** Minute i into `minute`: laid out for the day's listing first when it is not (zeros), then
     * its time and the underlying's quotes, each loaded chain's quotes, IVs and forward, each
     * expiry's time and rate; priced, its Greeks priced from the IVs when asked. A chain not loaded
     * stays zeros: fill a snapshot with this day's minutes only. */
    void fill(std::size_t i, model::PortfolioSnapshot& minute) const;

  private:
    struct Chain;

    /** What a minute's pricing needs of the underlying alone: its spot, each expiry's time. */
    void price_minutes();
    void load_chain(std::size_t c);
    /** Chain c priced, minute by minute. */
    void price_chain(std::size_t c, StoredChain& stored) const;

    utilities::LogSource log_;
    bool loaded_ = false;
    std::string error_;
    std::string path_;
    std::string cache_dir_;
    DayIndex index_; // its listing empty before a load
    double risk_free_rate_ = kRiskFreeRate;
    model::RateCurve rates_ = model::RateCurve::flat(kRiskFreeRate);
    std::string_view iv_price_mode_ = kIvPriceMode; // one of iv_price_mode_of's
    // Per minute, beside index_'s.
    std::vector<double> greeks_spot_;
    std::vector<double> expiry_years_; // a column per expiry
    std::vector<double> expiry_rate_;  // a column per expiry
    std::vector<std::unique_ptr<Chain>> chains_; // per chain of the listing
};

/** A loaded day, its chains loaded as wanted; read-only to the engines it is shared by, loaded
 * ahead while the day before runs. */
using LoadedDay = std::shared_ptr<HistoricalData>;

/** Load a day (HistoricalData::load) with these pricing settings, through cache_dir when given;
 * one that could not be loaded says why (has_data, load_error). */
LoadedDay load_day(const std::string& parquet_path, double risk_free_rate,
                   std::string_view iv_price_mode, const std::string& cache_dir = {});

/** What the pricing code makes of a fixed set of quotes, every value's bits hashed: the same as
 * long as pricing a stored day would give the same values. */
std::uint64_t pricing_fingerprint();

} // namespace adapters
