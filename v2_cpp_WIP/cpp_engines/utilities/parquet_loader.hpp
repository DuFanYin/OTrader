#pragma once

#include "types.hpp"
#include <arrow/api.h>
#include <concepts>
#include <cstdint>
#include <iterator>
#include <memory>
#include <ranges>
#include <string>
#include <unordered_map>
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

namespace detail {

inline auto ArrowTsToChrono(int64_t value, arrow::TimeUnit::type unit) -> Timestamp {
    using namespace std::chrono;
    std::chrono::system_clock::duration d;
    switch (unit) {
    case arrow::TimeUnit::SECOND:
        d = duration_cast<std::chrono::system_clock::duration>(seconds(value));
        break;
    case arrow::TimeUnit::MILLI:
        d = duration_cast<std::chrono::system_clock::duration>(milliseconds(value));
        break;
    case arrow::TimeUnit::MICRO:
        d = duration_cast<std::chrono::system_clock::duration>(microseconds(value));
        break;
    case arrow::TimeUnit::NANO:
    default:
        d = duration_cast<std::chrono::system_clock::duration>(nanoseconds(value));
        break;
    }
    return Timestamp{d};
}

inline auto ColumnChunk0(const arrow::Table* table, int col) -> const arrow::Array* {
    auto c = table->column(col);
    return c->num_chunks() > 0 ? c->chunk(0).get() : nullptr;
}

} // namespace detail

/** Callback: (frame) -> bool. Return false to stop iteration. Reused by ArrowParquetLoader and
 * BacktestDataEngine. */
template <typename F>
concept FramePredicate =
    std::invocable<F const&, TimestepFrameColumnar const&> &&
    std::convertible_to<std::invoke_result_t<F const&, TimestepFrameColumnar const&>, bool>;

/** Callback: (timestamp, frame) -> bool. Return false to stop. Used by
 * BacktestDataEngine::iter_timesteps. */
template <typename F>
concept TimestepFramePredicate =
    std::invocable<F const&, Timestamp, TimestepFrameColumnar const&> &&
    std::convertible_to<std::invoke_result_t<F const&, Timestamp, TimestepFrameColumnar const&>,
                        bool>;

/** Concrete Arrow-based parquet loader. No virtual interface; template iter_timesteps for
 * zero-erasure callbacks. */
class ArrowParquetLoader {
  public:
    [[nodiscard]] bool load(std::string const& path, std::string const& time_column = "ts_recv");
    [[nodiscard]] DataMeta get_meta() const;
    void collect_symbols(std::unordered_set<std::string>& out) const;

    /** Iterate (columnar frame) for each timestep. Callback returns false to stop. F is not
     * type-erased. */
    template <typename F>
        requires FramePredicate<F>
    void iter_timesteps(F&& fn) const {
        if (!table_ || time_col_index_ < 0) {
            return;
        }
        const arrow::Array* ts_arr = detail::ColumnChunk0(table_.get(), time_col_index_);
        if ((ts_arr == nullptr) || ts_arr->type_id() != arrow::Type::TIMESTAMP) {
            return;
        }

        const auto* ts = static_cast<const arrow::TimestampArray*>(ts_arr);
        auto ts_type = std::static_pointer_cast<arrow::TimestampType>(ts_arr->type());
        auto unit = ts_type->unit();
        const int64_t n = table_->num_rows();
        int col_sym = table_->schema()->GetFieldIndex("symbol");
        int col_bid = table_->schema()->GetFieldIndex("bid_px");
        int col_ask = table_->schema()->GetFieldIndex("ask_px");
        int col_bid_sz = table_->schema()->GetFieldIndex("bid_sz");
        int col_ask_sz = table_->schema()->GetFieldIndex("ask_sz");
        int col_ubid = table_->schema()->GetFieldIndex("underlying_bid_px");
        int col_uask = table_->schema()->GetFieldIndex("underlying_ask_px");
        int col_ubid_sz = table_->schema()->GetFieldIndex("underlying_bid_sz");
        int col_uask_sz = table_->schema()->GetFieldIndex("underlying_ask_sz");

        TimestepFrameColumnar frame;
        frame.arr_sym = col_sym >= 0 ? detail::ColumnChunk0(table_.get(), col_sym) : nullptr;
        frame.arr_bid_px = col_bid >= 0 ? detail::ColumnChunk0(table_.get(), col_bid) : nullptr;
        frame.arr_ask_px = col_ask >= 0 ? detail::ColumnChunk0(table_.get(), col_ask) : nullptr;
        frame.arr_bid_sz =
            col_bid_sz >= 0 ? detail::ColumnChunk0(table_.get(), col_bid_sz) : nullptr;
        frame.arr_ask_sz =
            col_ask_sz >= 0 ? detail::ColumnChunk0(table_.get(), col_ask_sz) : nullptr;
        frame.arr_underlying_bid_px =
            col_ubid >= 0 ? detail::ColumnChunk0(table_.get(), col_ubid) : nullptr;
        frame.arr_underlying_ask_px =
            col_uask >= 0 ? detail::ColumnChunk0(table_.get(), col_uask) : nullptr;
        frame.arr_underlying_bid_sz =
            col_ubid_sz >= 0 ? detail::ColumnChunk0(table_.get(), col_ubid_sz) : nullptr;
        frame.arr_underlying_ask_sz =
            col_uask_sz >= 0 ? detail::ColumnChunk0(table_.get(), col_uask_sz) : nullptr;

        bool non_decreasing = true;
        for (int64_t i = 1; i < n; ++i) {
            if (ts->Value(i) < ts->Value(i - 1)) {
                non_decreasing = false;
                break;
            }
        }

        if (non_decreasing) {
            int64_t i = 0;
            while (i < n) {
                const int64_t t_val = ts->Value(i);
                int64_t j = i + 1;
                while (j < n && ts->Value(j) == t_val) {
                    ++j;
                }

                frame.timestamp = detail::ArrowTsToChrono(t_val, unit);
                frame.num_rows = j - i;
                frame.start_row = i;
                frame.row_indices.clear();
                if (!std::invoke(fn, frame)) {
                    break;
                }
                i = j;
            }
            return;
        }

        std::unordered_map<int64_t, std::vector<int64_t>> groups;
        groups.reserve(static_cast<size_t>(n / 4));
        for (int64_t i = 0; i < n; ++i) {
            groups[ts->Value(i)].push_back(i);
        }

        std::vector<int64_t> sorted_ts;
        sorted_ts.reserve(groups.size());
        std::ranges::copy(std::views::keys(groups), std::back_inserter(sorted_ts));
        std::ranges::sort(sorted_ts);

        for (int64_t t_val : sorted_ts) {
            frame.timestamp = detail::ArrowTsToChrono(t_val, unit);
            frame.row_indices = groups[t_val];
            frame.num_rows = static_cast<int64_t>(frame.row_indices.size());
            frame.start_row = 0;
            if (!std::invoke(fn, frame)) {
                break;
            }
        }
    }

  private:
    DataMeta meta_;
    std::shared_ptr<arrow::Table> table_;
    int time_col_index_ = -1;
};

std::unique_ptr<ArrowParquetLoader> make_parquet_loader();

} // namespace backtest
