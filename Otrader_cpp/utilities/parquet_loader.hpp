#pragma once

#include "types.hpp"
#include <arrow/api.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace backtest {

/**
 * Columnar timestep frame (zero-copy view over Arrow columns).
 * Consumer reads by row index from column arrays; no per-row allocation.
 * Arrays are valid for the duration of the loader's iter_timesteps callback.
 */
struct TimestepFrameColumnar {
    Timestamp timestamp{};
    int64_t num_rows = 0;
    /// When row_indices is empty, logical row r is table row (start_row + r).
    int64_t start_row = 0;
    /// When non-empty, logical row r is table row row_indices[r]; num_rows == row_indices.size().
    std::vector<int64_t> row_indices;

    const arrow::Array* arr_sym = nullptr;
    const arrow::Array* arr_bid_px = nullptr;
    const arrow::Array* arr_ask_px = nullptr;
    const arrow::Array* arr_bid_sz = nullptr;
    const arrow::Array* arr_ask_sz = nullptr;
    const arrow::Array* arr_underlying_bid_px = nullptr;
    const arrow::Array* arr_underlying_ask_px = nullptr;
    const arrow::Array* arr_underlying_bid_sz = nullptr;
    const arrow::Array* arr_underlying_ask_sz = nullptr;

    /// Logical row index r (0..num_rows-1) -> physical table row index.
    [[nodiscard]] int64_t row_index(int64_t r) const {
        return row_indices.empty() ? (start_row + r) : row_indices[static_cast<size_t>(r)];
    }
};

// Interface for loading parquet and iterating timesteps (C++20)
class IParquetLoader {
  public:
    virtual ~IParquetLoader() = default;

    // Load parquet from path (relative to project root or absolute). Returns true on success.
    [[nodiscard]] virtual bool load(std::string const& path,
                                    std::string const& time_column = "ts_recv") = 0;

    [[nodiscard]] virtual DataMeta get_meta() const = 0;

    // Collect all unique symbol values by scanning only the symbol column (no row-wise copy).
    // Call after load(). Fills out with non-empty symbol strings.
    virtual void collect_symbols(std::unordered_set<std::string>& out) const = 0;

    // Iterate (columnar frame) for each timestep. Zero-copy: frame holds column Array* +
    // start_row/num_rows or row_indices. Callback returns false to stop. Frame arrays valid only
    // for the duration of the callback.
    virtual void
    iter_timesteps(std::function<bool(TimestepFrameColumnar const&)> const& fn) const = 0;
};

// Factory: returns Arrow-based loader if available, else stub that fails load.
std::unique_ptr<IParquetLoader> make_parquet_loader();

} // namespace backtest
