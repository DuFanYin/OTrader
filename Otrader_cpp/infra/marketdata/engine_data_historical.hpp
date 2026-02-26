#pragma once

#include "constant.hpp"
#include "object.hpp"
#include "parquet_loader.hpp"
#include "portfolio.hpp"
#include "types.hpp"
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace backtest {

class MainEngine;

class BacktestDataEngine {
  public:
    explicit BacktestDataEngine(MainEngine* main_engine = nullptr);

    // Load parquet from path (relative to project root or absolute).
    // time_column default "ts_recv"; underlying_symbol optional (inferred from filename if empty).
    // This resets the portfolio and builds it from scratch.
    void load_parquet(std::string const& rel_path, std::string const& time_column = "ts_recv",
                      std::string const& underlying_symbol = "");

    [[nodiscard]] DataMeta get_meta() const;

    // Iterate (timestamp, columnar frame) for each timestep. Callback returns false to stop.
    void
    iter_timesteps(std::function<bool(Timestamp, TimestepFrameColumnar const&)> const& fn) const;

    void set_risk_free_rate(double rate);
    void set_iv_price_mode(std::string mode);
    [[nodiscard]] double risk_free_rate() const { return risk_free_rate_; }
    [[nodiscard]] const std::string& iv_price_mode() const { return iv_price_mode_; }

    [[nodiscard]] std::optional<BacktestPortfolio> const& portfolio() const { return portfolio_; }
    [[nodiscard]] bool has_data() const { return loader_ != nullptr && loaded_; }
    utilities::PortfolioData* portfolio_data() const { return portfolio_data_.get(); }

    /** After load, snapshots are precomputed (one per frame). Run uses these; no per-frame
     * IV/Greeks. */
    [[nodiscard]] size_t get_precomputed_snapshot_count() const { return snapshots_.size(); }
    [[nodiscard]] utilities::PortfolioSnapshot const& get_precomputed_snapshot(size_t i) const {
        return snapshots_.at(i);
    }
    /** Apply precomputed snapshot to portfolio_data_ (for tests or when not using event). */
    void apply_precomputed_snapshot(size_t i);

  private:
    void build_portfolio_from_symbols(std::vector<std::string> const& symbols);
    void create_portfolio_data(std::vector<std::string> const& symbols,
                               std::optional<utilities::DateTime> dte_ref = std::nullopt);
    void build_option_apply_index();
    /** One-time: parquet OCC symbol -> OptionData* (so build_snapshot does not parse or
     * options.find per row). */
    void build_occ_to_option(std::unordered_set<std::string> const& occ_symbols);
    /** Build one snapshot from frame; if prev is non-null, copy its values first so options not in
     * this frame keep last state. */
    utilities::PortfolioSnapshot
    build_snapshot_from_frame(TimestepFrameColumnar const& frame,
                              utilities::PortfolioSnapshot const* prev = nullptr);
    void precompute_snapshots();

    MainEngine* main_engine_ = nullptr;
    std::unique_ptr<IParquetLoader> loader_;
    bool loaded_ = false;
    std::string time_column_;
    std::string underlying_symbol_;
    std::optional<BacktestPortfolio> portfolio_;
    std::unique_ptr<utilities::PortfolioData> portfolio_data_;
    std::unordered_map<std::string, std::string> occ_to_standard_symbol_;
    /** One-time at load: parquet raw (OCC) symbol -> OptionData*; build phase only does find, no
     * parse. */
    std::unordered_map<std::string, utilities::OptionData*> occ_to_option_;
    double risk_free_rate_ = 0.05;
    std::string iv_price_mode_ = "mid"; // "mid" | "bid" | "ask"
    std::vector<utilities::PortfolioSnapshot> snapshots_;
    std::unordered_map<utilities::OptionData*, size_t> option_apply_index_;
};

} // namespace backtest
