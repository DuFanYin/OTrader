#include "parquet_loader.hpp"
#include <algorithm>
#include <array>
#include <arrow/api.h>
#include <arrow/io/api.h>
#include <chrono>
#include <filesystem>
#include <iterator>
#include <parquet/arrow/reader.h>
#include <ranges>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace backtest {

namespace {

using namespace arrow;
using namespace parquet::arrow;

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

} // namespace

// Column types the frame readers cast to (engine_data_historical.cpp). A column in the wrong
// type would be static_cast to the wrong Arrow array class, so it is rejected at load.
auto CheckSchema(Schema const& schema, std::string const& time_column) -> std::string {
    const auto require = [&](std::string const& name, std::initializer_list<Type::type> ok,
                             bool required) -> std::string {
        const auto field = schema.GetFieldByName(name);
        if (!field) {
            return required ? "missing column '" + name + "'" : std::string{};
        }
        const Type::type id = field->type()->id();
        if (std::ranges::find(ok, id) == ok.end()) {
            return "column '" + name + "' has type " + field->type()->ToString();
        }
        return {};
    };
    for (auto const& problem : {
             require(time_column, {Type::TIMESTAMP}, true),
             require("symbol", {Type::STRING, Type::LARGE_STRING}, true),
             require("bid_px", {Type::DOUBLE}, false),
             require("ask_px", {Type::DOUBLE}, false),
             require("bid_sz", {Type::INT64}, false),
             require("ask_sz", {Type::INT64}, false),
             require("underlying_bid_px", {Type::DOUBLE}, false),
             require("underlying_ask_px", {Type::DOUBLE}, false),
             require("underlying_bid_sz", {Type::INT64}, false),
             require("underlying_ask_sz", {Type::INT64}, false),
         }) {
        if (!problem.empty()) {
            return problem;
        }
    }
    return {};
}

bool ArrowParquetLoader::load(std::string const& path, std::string const& time_column) {
    meta_ = DataMeta{};
    meta_.path = path;
    meta_.time_column = time_column;
    table_.reset();
    time_col_index_ = -1;
    error_.clear();

    std::string resolved = path;
    if (!std::filesystem::path(path).is_absolute()) {
        std::filesystem::path cwd = std::filesystem::current_path();
        if (cwd.filename() == "build") {
            cwd = cwd.parent_path();
        }
        resolved = (cwd / path).string();
    }

    // Memory-mapped open (efficient)
    std::shared_ptr<io::MemoryMappedFile> infile;
    auto status = io::MemoryMappedFile::Open(resolved, io::FileMode::READ);
    if (!status.ok()) {
        error_ = "cannot open " + resolved + ": " + status.status().ToString();
        return false;
    }
    infile = *status;

    // Parquet reader
    auto reader_result = OpenFile(infile, default_memory_pool());
    if (!reader_result.ok()) {
        error_ = "not a Parquet file: " + reader_result.status().ToString();
        return false;
    }
    std::unique_ptr<FileReader> reader = std::move(reader_result).ValueOrDie();

    // Read table
    std::shared_ptr<Table> table;
    PARQUET_THROW_NOT_OK(reader->ReadTable(&table));
    if (!table) {
        error_ = "empty table";
        return false;
    }
    if (error_ = CheckSchema(*table->schema(), time_column); !error_.empty()) {
        return false;
    }
    // Readers index rows 0..num_rows-1 on chunk 0 of every column; make that the whole column.
    auto combined = table->CombineChunks(default_memory_pool());
    if (!combined.ok()) {
        error_ = "cannot combine chunks: " + combined.status().ToString();
        return false;
    }
    table_ = *combined;

    meta_.row_count = table_->num_rows();
    time_col_index_ = table_->schema()->GetFieldIndex(time_column);

    if (meta_.row_count > 0) {
        const Array* ts_arr = detail::ColumnChunk0(table_.get(), time_col_index_);
        if ((ts_arr != nullptr) && ts_arr->type_id() == Type::TIMESTAMP) {
            const auto* ts = static_cast<const TimestampArray*>(ts_arr);
            auto ts_type = std::static_pointer_cast<TimestampType>(ts_arr->type());
            auto unit = ts_type->unit();
            meta_.ts_start = TsToIso(detail::ArrowTsToChrono(ts->Value(0), unit));
            meta_.ts_end = TsToIso(detail::ArrowTsToChrono(ts->Value(ts->length() - 1), unit));
        }
    }
    return true;
}

auto ArrowParquetLoader::get_meta() const -> DataMeta { return meta_; }

void ArrowParquetLoader::collect_symbols(std::unordered_set<std::string>& out) const {
    if (!table_) {
        return;
    }
    const int col_sym = table_->schema()->GetFieldIndex("symbol");
    if (col_sym < 0) {
        return;
    }
    const Array* arr = detail::ColumnChunk0(table_.get(), col_sym);
    if ((arr == nullptr) || arr->null_count() == arr->length()) {
        return;
    }
    const int64_t n = arr->length();
    for (int64_t i = 0; i < n; ++i) {
        const std::string_view s = detail::StringAt(arr, i);
        if (!s.empty()) {
            out.emplace(s);
        }
    }
}

auto make_parquet_loader() -> std::unique_ptr<ArrowParquetLoader> {
    return std::make_unique<ArrowParquetLoader>();
}

} // namespace backtest
