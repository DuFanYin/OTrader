#include "parquet_loader.hpp"
#include <algorithm>
#include <array>
#include <arrow/api.h>
#include <arrow/io/api.h>
#include <chrono>
#include <filesystem>
#include <parquet/arrow/reader.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace backtest {

namespace {

using namespace arrow;
using namespace parquet::arrow;

auto ArrowTsToChrono(int64_t value, TimeUnit::type unit) -> Timestamp {
    using namespace std::chrono;
    system_clock::duration d;
    switch (unit) {
    case TimeUnit::SECOND:
        d = duration_cast<system_clock::duration>(seconds(value));
        break;
    case TimeUnit::MILLI:
        d = duration_cast<system_clock::duration>(milliseconds(value));
        break;
    case TimeUnit::MICRO:
        d = duration_cast<system_clock::duration>(microseconds(value));
        break;
    case TimeUnit::NANO:
    default:
        d = duration_cast<system_clock::duration>(nanoseconds(value));
        break;
    }
    return Timestamp{d};
}

auto TsToIso(Timestamp ts) -> std::string {
    auto t = std::chrono::system_clock::to_time_t(ts);
    std::tm* tm = std::gmtime(&t);
    if (tm == nullptr) {
        return "";
    }
    std::array<char, 32> buf{};
    std::snprintf(buf.data(), buf.size(), "%04d-%02d-%02dT%02d:%02d:%02dZ", tm->tm_year + 1900,
                  tm->tm_mon + 1, tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec);
    return buf.data();
}

// 直接单 chunk：列只读 chunk(0)，行索引直接用 i
inline auto ColumnChunk0(const Table* table, int col) -> const Array* {
    auto c = table->column(col);
    return c->num_chunks() > 0 ? c->chunk(0).get() : nullptr;
}

} // namespace

class ArrowParquetLoader : public IParquetLoader {
  public:
    auto load(std::string const& path, std::string const& time_column) -> bool override {
        meta_.path = path;
        meta_.time_column = time_column;
        table_.reset();
        time_col_index_ = -1;

        std::string resolved = path;
        if (!std::filesystem::path(path).is_absolute()) {
            std::filesystem::path cwd = std::filesystem::current_path();
            if (cwd.filename() == "build") {
                cwd = cwd.parent_path();
            }
            resolved = (cwd / path).string();
        }

        // 1) 使用内存映射打开文件（更高效，避免一次性加载到内存）
        std::shared_ptr<io::MemoryMappedFile> infile;
        auto status = io::MemoryMappedFile::Open(resolved, io::FileMode::READ);
        if (!status.ok()) {
            return false;
        }
        infile = *status;

        // 2) 创建 Parquet reader（Arrow 新 API：OpenFile 两参数，返回
        // Result<unique_ptr<FileReader>>）
        auto reader_result = OpenFile(infile, default_memory_pool());
        if (!reader_result.ok()) {
            return false;
        }
        std::unique_ptr<FileReader> reader = std::move(reader_result).ValueOrDie();

        // 3) 读整个文件为 Arrow Table（ReadTable 仍为出参形式）
        std::shared_ptr<Table> table;
        PARQUET_THROW_NOT_OK(reader->ReadTable(&table));
        if (!table) {
            return false;
        }
        table_ = table;

        meta_.row_count = table_->num_rows();
        auto* schema = table_->schema().get();
        time_col_index_ = schema->GetFieldIndex(time_column);
        if (time_col_index_ < 0) {
            return false;
        }

        if (meta_.row_count > 0) {
            const Array* ts_arr = ColumnChunk0(table_.get(), time_col_index_);
            if ((ts_arr != nullptr) && ts_arr->type_id() == Type::TIMESTAMP) {
                const auto* ts = static_cast<const TimestampArray*>(ts_arr);
                auto ts_type = std::static_pointer_cast<TimestampType>(ts_arr->type());
                auto unit = ts_type->unit();
                meta_.ts_start = TsToIso(ArrowTsToChrono(ts->Value(0), unit));
                meta_.ts_end = TsToIso(ArrowTsToChrono(ts->Value(ts->length() - 1), unit));
            }
        }
        return true;
    }

    [[nodiscard]] auto get_meta() const -> DataMeta override { return meta_; }

    void collect_symbols(std::unordered_set<std::string>& out) const override {
        if (!table_) {
            return;
        }
        const int col_sym = table_->schema()->GetFieldIndex("symbol");
        if (col_sym < 0) {
            return;
        }
        const Array* arr = ColumnChunk0(table_.get(), col_sym);
        if ((arr == nullptr) || arr->type_id() != Type::STRING ||
            arr->null_count() == arr->length()) {
            return;
        }
        const auto* str_arr = static_cast<const StringArray*>(arr);
        const int64_t n = arr->length();
        for (int64_t i = 0; i < n; ++i) {
            if (arr->IsNull(i)) {
                continue;
            }
            std::string s = str_arr->GetString(i);
            if (!s.empty()) {
                out.insert(std::move(s));
            }
        }
    }

    void
    iter_timesteps(std::function<bool(TimestepFrameColumnar const&)> const& fn) const override {
        if (!table_ || time_col_index_ < 0) {
            return;
        }
        const Array* ts_arr = ColumnChunk0(table_.get(), time_col_index_);
        if ((ts_arr == nullptr) || ts_arr->type_id() != Type::TIMESTAMP) {
            return;
        }

        const auto* ts = static_cast<const TimestampArray*>(ts_arr);
        auto ts_type = std::static_pointer_cast<TimestampType>(ts_arr->type());
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
        frame.arr_sym = col_sym >= 0 ? ColumnChunk0(table_.get(), col_sym) : nullptr;
        frame.arr_bid_px = col_bid >= 0 ? ColumnChunk0(table_.get(), col_bid) : nullptr;
        frame.arr_ask_px = col_ask >= 0 ? ColumnChunk0(table_.get(), col_ask) : nullptr;
        frame.arr_bid_sz = col_bid_sz >= 0 ? ColumnChunk0(table_.get(), col_bid_sz) : nullptr;
        frame.arr_ask_sz = col_ask_sz >= 0 ? ColumnChunk0(table_.get(), col_ask_sz) : nullptr;
        frame.arr_underlying_bid_px =
            col_ubid >= 0 ? ColumnChunk0(table_.get(), col_ubid) : nullptr;
        frame.arr_underlying_ask_px =
            col_uask >= 0 ? ColumnChunk0(table_.get(), col_uask) : nullptr;
        frame.arr_underlying_bid_sz =
            col_ubid_sz >= 0 ? ColumnChunk0(table_.get(), col_ubid_sz) : nullptr;
        frame.arr_underlying_ask_sz =
            col_uask_sz >= 0 ? ColumnChunk0(table_.get(), col_uask_sz) : nullptr;

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

                frame.timestamp = ArrowTsToChrono(t_val, unit);
                frame.num_rows = j - i;
                frame.start_row = i;
                frame.row_indices.clear();
                if (!fn(frame)) {
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
        for (auto const& [t, _] : groups) {
            sorted_ts.push_back(t);
        }
        std::ranges::sort(sorted_ts);

        for (int64_t t_val : sorted_ts) {
            frame.timestamp = ArrowTsToChrono(t_val, unit);
            frame.row_indices = groups[t_val];
            frame.num_rows = static_cast<int64_t>(frame.row_indices.size());
            frame.start_row = 0;
            if (!fn(frame)) {
                break;
            }
        }
    }

  private:
    DataMeta meta_;
    std::shared_ptr<Table> table_;
    int time_col_index_ = -1;
};

auto make_parquet_loader() -> std::unique_ptr<IParquetLoader> {
    return std::make_unique<ArrowParquetLoader>();
}

} // namespace backtest
