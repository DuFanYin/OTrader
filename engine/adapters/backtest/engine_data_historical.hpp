#pragma once

#include "../../model/portfolio_structure.hpp"
#include "../../utilities/base_engine.hpp"
#include "backtest_types.hpp"
#include "../../model/constant.hpp"
#include "../../model/object.hpp"
#include "parquet_loader.hpp"
#include "../../model/portfolio.hpp"
#include <concepts>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace adapters {

class BacktestDataEngine : public utilities::BaseEngine {
  public:
    /** Fills `portfolios` with the day's contracts: one portfolio, named after their root (the
     * live engine's set-up when it trades it, model::kPortfolios). */
    BacktestDataEngine(utilities::Logger* logger, model::PortfolioStructure* portfolios);

    /** Load a day; its portfolio is `underlying_symbol` (empty: from the path, data/<ROOT>/...). */
    void load_parquet(std::string const& rel_path, std::string const& time_column = "ts_recv",
                      std::string const& underlying_symbol = "");
    /** The day's portfolio, as load_parquet named it. */
    [[nodiscard]] const std::string& portfolio_name() const { return portfolio_name_; }

    [[nodiscard]] DataMeta get_meta() const;

    // Iterate timesteps; callback returns false to stop
    template <typename F>
        requires adapters::TimestepFramePredicate<F>
    void iter_timesteps(F&& fn) const {
        if (!loaded_) {
            return;
        }
        loader_.iter_timesteps([&fn](TimestepFrameColumnar const& frame) -> bool {
            return std::invoke(fn, frame.timestamp, frame);
        });
    }

    void set_risk_free_rate(double rate);
    void set_iv_price_mode(std::string mode);
    [[nodiscard]] double risk_free_rate() const { return risk_free_rate_; }
    [[nodiscard]] const std::string& iv_price_mode() const { return iv_price_mode_; }

    [[nodiscard]] bool has_data() const { return loaded_; }
    /** Why the last load_parquet failed (empty after a successful load). */
    [[nodiscard]] std::string const& load_error() const { return loader_.last_error(); }
    model::PortfolioData* portfolio_data() const;

    /** Precomputed snapshots (one per frame). */
    [[nodiscard]] size_t get_precomputed_snapshot_count() const { return snapshots_.size(); }
    [[nodiscard]] model::PortfolioSnapshot const& get_precomputed_snapshot(size_t i) const {
        return snapshots_.at(i);
    }
    /** Apply precomputed snapshot. */
    void apply_precomputed_snapshot(size_t i);
    /** Price every precomputed snapshot (PortfolioData::price_snapshot) with the portfolio's
     * pricing settings as they are now, the days' bars in parallel: a run applies them priced. */
    void price_precomputed_snapshots();

  private:
    void create_portfolio_data(std::vector<std::string> const& symbols,
                               std::optional<model::DateTime> dte_ref = std::nullopt);
    /** Each OCC code the day has, to its option's column (option_apply_order). */
    void build_occ_columns(std::unordered_set<std::string> const& occ_symbols);
    /** Build snapshot from frame; prev keeps last state. */
    model::PortfolioSnapshot
    build_snapshot_from_frame(TimestepFrameColumnar const& frame,
                              model::PortfolioSnapshot const* prev = nullptr);
    void precompute_snapshots();

    ArrowParquetLoader loader_;
    bool loaded_ = false;
    std::string time_column_;
    std::string portfolio_name_; // the day's options' root
    std::unordered_map<std::string, size_t> occ_columns_; // OCC code -> column
    double risk_free_rate_ = 0.05;
    std::string iv_price_mode_ = "mid";
    model::PortfolioStructure* portfolios_ = nullptr;
    std::vector<model::PortfolioSnapshot> snapshots_;
};

} // namespace adapters
