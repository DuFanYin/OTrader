#include "adapters/market_data/data_loader.hpp"
#include "model/portfolio_spec.hpp"
#include "model/symbol.hpp"
#include "model/utility.hpp"
#include "utilities/fnv.hpp"
#include <algorithm>
#include <array>
#include <arrow/api.h>
#include <arrow/array/array_dict.h>
#include <arrow/io/api.h>
#include <arrow/util/key_value_metadata.h>
#include <bit>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <memory>
#include <nlohmann/json.hpp>
#include <numeric>
#include <optional>
#include <parquet/arrow/reader.h>
#include <parquet/file_reader.h>
#include <parquet/metadata.h>
#include <parquet/statistics.h>
#include <print>
#include <ranges>
#include <stdexcept>
#include <string_view>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <type_traits>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <vector>

namespace adapters {

// ---- What the files hold
namespace {

constexpr const char* kTimeColumn = "ts_recv";

/** A price column's types: a double, or a decimal (an integer and its number of decimal places,
 * as the data vendors send prices) of up to 18 digits. */
constexpr std::array kPriceTypes{arrow::Type::DOUBLE, arrow::Type::DECIMAL32, arrow::Type::DECIMAL64,
                                 arrow::Type::DECIMAL128};

/** What is wrong with the schema for a day file: a column missing or of another type (it would be
 * read as the wrong Arrow array); empty when nothing is. */
auto check_schema(const arrow::Schema& schema) -> std::string {
    using arrow::Type;
    const auto problem = [&](const char* name, std::span<const Type::type> types,
                             bool required) -> std::string {
        const auto field = schema.GetFieldByName(name);
        if (!field) {
            return required ? std::format("missing column '{}'", name) : std::string{};
        }
        const arrow::DataType& type =
            field->type()->id() == Type::DICTIONARY // a string column read by its dictionary
                ? *static_cast<const arrow::DictionaryType&>(*field->type()).value_type()
                : *field->type();
        if (!std::ranges::contains(types, type.id())) {
            return std::format("column '{}' has type {}", name, field->type()->ToString());
        }
        if (const auto* decimal = dynamic_cast<const arrow::DecimalType*>(field->type().get());
            decimal != nullptr && (decimal->precision() > 18 || decimal->scale() < 0)) {
            return std::format("column '{}' has type {}", name, field->type()->ToString());
        }
        return {};
    };
    constexpr std::array kTime{Type::TIMESTAMP};
    constexpr std::array kString{Type::STRING, Type::LARGE_STRING};
    constexpr std::array kInt{Type::INT64};
    for (std::string why : {
             problem(kTimeColumn, kTime, true),
             problem("symbol", kString, true),
             problem("bid_px", kPriceTypes, false),
             problem("ask_px", kPriceTypes, false),
             problem("bid_sz", kInt, false),
             problem("ask_sz", kInt, false),
             problem("underlying_bid_px", kPriceTypes, false),
             problem("underlying_ask_px", kPriceTypes, false),
         }) {
        if (!why.empty()) {
            return why;
        }
    }
    return {};
}

/** A column of the table (its chunks combined into one), none when it has no such column. */
auto column(const arrow::Table& table, const char* name) -> const arrow::Array* {
    const auto c = table.GetColumnByName(name);
    return c != nullptr && c->num_chunks() > 0 ? c->chunk(0).get() : nullptr;
}

/** 10 to the power of minus its index, as one double each (0.01 for two decimal places). */
constexpr std::array<double, 19> kTenToMinus{1.0,   1e-1,  1e-2,  1e-3,  1e-4,  1e-5,  1e-6,
                                             1e-7,  1e-8,  1e-9,  1e-10, 1e-11, 1e-12, 1e-13,
                                             1e-14, 1e-15, 1e-16, 1e-17, 1e-18};

/** Row `row` of a price column (kPriceTypes); `otherwise` when null or the file has no such column.
 * A decimal is its integer times 10^-places, the double the data vendors' own clients make of it
 * (9332 with two places is 9332 * 0.01, 93.32000000000001). */
static_assert(std::endian::native == std::endian::little, "decimals are read as little-endian");

auto price_at(const arrow::Array* a, std::int64_t row, double otherwise) -> double {
    if (a == nullptr || a->IsNull(row)) {
        return otherwise;
    }
    if (a->type_id() == arrow::Type::DOUBLE) {
        return static_cast<const arrow::DoubleArray*>(a)->Value(row);
    }
    // A decimal is a little-endian two's complement integer of 4, 8 or 16 bytes; up to 18 digits
    // fit the low 8 of the 16.
    const std::uint8_t* bytes = static_cast<const arrow::FixedSizeBinaryArray*>(a)->GetValue(row);
    std::int64_t integer = 0;
    if (a->type_id() == arrow::Type::DECIMAL32) {
        std::int32_t small = 0;
        std::memcpy(&small, bytes, sizeof small);
        integer = small;
    } else {
        std::memcpy(&integer, bytes, sizeof integer);
    }
    const int places = static_cast<const arrow::DecimalType&>(*a->type()).scale();
    return static_cast<double>(integer) * kTenToMinus[static_cast<std::size_t>(places)];
}

/** Row `row` of an int64 column, as a double; 0 when null or the file has no such column. */
auto size_at(const arrow::Array* a, std::int64_t row) -> double {
    return a != nullptr && !a->IsNull(row)
               ? static_cast<double>(static_cast<const arrow::Int64Array*>(a)->Value(row))
               : 0.0;
}

/** Row `row` of a string column (pyarrow writes string, polars large_string), or of one read by
 * its dictionary; empty when null. */
auto string_at(const arrow::Array* a, std::int64_t row) -> std::string_view {
    if (a == nullptr || a->IsNull(row)) {
        return {};
    }
    if (a->type_id() == arrow::Type::DICTIONARY) {
        const auto* d = static_cast<const arrow::DictionaryArray*>(a);
        return string_at(d->dictionary().get(), d->GetValueIndex(row));
    }
    return a->type_id() == arrow::Type::LARGE_STRING
               ? static_cast<const arrow::LargeStringArray*>(a)->GetView(row)
               : static_cast<const arrow::StringArray*>(a)->GetView(row);
}

/** A symbol column as read (by its dictionary): each row's place in `values`, the strings each
 * once. A day file has a few hundred to a few thousand contracts and millions of rows: a row is
 * looked up by its place, not hashed as text. */
struct Symbols {
    const arrow::DictionaryArray* column;
    const arrow::Array* values;

    explicit Symbols(const arrow::Array* a)
        : column(static_cast<const arrow::DictionaryArray*>(a)),
          values(column->dictionary().get()) {}
    [[nodiscard]] std::int64_t size() const { return values->length(); }
    /** Row `row`'s place in values; -1 when null. */
    [[nodiscard]] std::int64_t at(std::int64_t row) const {
        return column->IsNull(row) ? -1 : column->GetValueIndex(row);
    }
    [[nodiscard]] std::string_view value(std::int64_t i) const { return string_at(values, i); }
    /** The values some row has, sorted. */
    [[nodiscard]] std::vector<std::string> used() const {
        std::vector<bool> seen(static_cast<std::size_t>(size()), false);
        for (std::int64_t row = 0; row < column->length(); ++row) {
            if (const std::int64_t i = at(row); i >= 0) {
                seen[static_cast<std::size_t>(i)] = true;
            }
        }
        std::vector<std::string> out;
        for (std::int64_t i = 0; i < size(); ++i) {
            if (seen[static_cast<std::size_t>(i)]) {
                out.emplace_back(value(i));
            }
        }
        std::ranges::sort(out);
        return out;
    }
};

auto to_datetime(std::int64_t value, arrow::TimeUnit::type unit) -> model::DateTime {
    using namespace std::chrono;
    switch (unit) {
    case arrow::TimeUnit::SECOND:
        return model::DateTime{duration_cast<system_clock::duration>(seconds(value))};
    case arrow::TimeUnit::MILLI:
        return model::DateTime{duration_cast<system_clock::duration>(milliseconds(value))};
    case arrow::TimeUnit::MICRO:
        return model::DateTime{duration_cast<system_clock::duration>(microseconds(value))};
    case arrow::TimeUnit::NANO:
        break;
    }
    return model::DateTime{duration_cast<system_clock::duration>(nanoseconds(value))};
}

/** A relative path from the repository: the working directory, or its parent from build/. */
auto resolve_path(const std::string& path) -> std::string {
    namespace fs = std::filesystem;
    if (fs::path(path).is_absolute()) {
        return path;
    }
    fs::path cwd = fs::current_path();
    if (cwd.filename() == "build") {
        cwd = cwd.parent_path();
    }
    return (cwd / path).string();
}

/** The risk-free curve a day file carries: its key-value metadata risk_free_curve, {"years": [...],
 * "rates": [...]} (continuously compounded, a year; the years increasing); none when it carries
 * none or not one. */
auto rates_of(const arrow::Table& table) -> std::optional<model::RateCurve> {
    const std::shared_ptr<const arrow::KeyValueMetadata>& meta = table.schema()->metadata();
    if (meta == nullptr) {
        return std::nullopt;
    }
    const auto text = meta->Get("risk_free_curve");
    if (!text.ok()) {
        return std::nullopt;
    }
    const nlohmann::json curve = nlohmann::json::parse(*text, nullptr, /*allow_exceptions=*/false);
    if (!curve.is_object() || !curve.contains("years") || !curve.contains("rates") ||
        !curve["years"].is_array() || !curve["rates"].is_array()) {
        return std::nullopt;
    }
    model::RateCurve out{.years = {}, .rates = {}};
    for (const auto& [to, name] : {std::pair{&out.years, "years"}, std::pair{&out.rates, "rates"}}) {
        for (const nlohmann::json& v : curve[name]) {
            if (!v.is_number() || !std::isfinite(v.get<double>())) {
                return std::nullopt;
            }
            to->push_back(v.get<double>());
        }
    }
    if (out.years.empty() || out.years.size() != out.rates.size()) {
        return std::nullopt;
    }
    for (std::size_t j = 1; j < out.years.size(); ++j) {
        if (!(out.years[j] > out.years[j - 1])) {
            return std::nullopt;
        }
    }
    return out;
}

/** The columns a day file's quotes are read from: those check_schema knows. */
constexpr std::array<const char*, 8> kQuoteColumnNames{
    kTimeColumn,  "symbol", "bid_px", "ask_px", "bid_sz", "ask_sz", "underlying_bid_px",
    "underlying_ask_px"};

/** Whether to read a row group, from its symbols' first and last (by its statistics). */
using RowGroupFilter = std::function<bool(std::string_view first, std::string_view last)>;

/** Of a day file, these columns (those it has) of the row groups `keep` takes (all when none, and
 * any whose statistics say nothing), their chunks combined: a row is an index into a column. */
auto read_table(const std::string& path, std::span<const char* const> names,
                const RowGroupFilter& keep = {})
    -> std::expected<std::shared_ptr<arrow::Table>, std::string> {
    const std::string file = resolve_path(path);
    auto opened = arrow::io::MemoryMappedFile::Open(file, arrow::io::FileMode::READ);
    if (!opened.ok()) {
        return std::unexpected("cannot open " + file + ": " + opened.status().ToString());
    }
    parquet::arrow::FileReaderBuilder builder;
    if (const arrow::Status open = builder.Open(*opened); !open.ok()) {
        return std::unexpected("not a Parquet file: " + open.ToString());
    }
    parquet::ArrowReaderProperties properties = parquet::default_arrow_reader_properties();
    properties.set_use_threads(true); // the columns decoded at once
    if (const int symbol = builder.raw_reader()->metadata()->schema()->ColumnIndex("symbol");
        symbol >= 0) {
        properties.set_read_dictionary(symbol, true); // as written: no string a row
    }
    auto reader = builder.properties(properties)->Build();
    if (!reader.ok()) {
        return std::unexpected("not a Parquet file: " + reader.status().ToString());
    }
    std::shared_ptr<arrow::Schema> schema;
    if (const arrow::Status read = (*reader)->GetSchema(&schema); !read.ok() || !schema) {
        return std::unexpected("cannot read the schema: " + read.ToString());
    }
    if (std::string why = check_schema(*schema); !why.empty()) {
        return std::unexpected(std::move(why));
    }
    std::vector<int> columns;
    for (const char* name : names) {
        if (const int i = schema->GetFieldIndex(name); i >= 0) {
            columns.push_back(i);
        }
    }
    const parquet::FileMetaData& meta = *(*reader)->parquet_reader()->metadata();
    const int symbol = meta.schema()->ColumnIndex("symbol");
    std::vector<int> groups;
    for (int g = 0; g < meta.num_row_groups(); ++g) {
        const std::shared_ptr<parquet::Statistics> stats =
            symbol >= 0 ? meta.RowGroup(g)->ColumnChunk(symbol)->statistics() : nullptr;
        if (keep && stats != nullptr && stats->HasMinMax() &&
            stats->physical_type() == parquet::Type::BYTE_ARRAY) {
            const auto& bytes = static_cast<const parquet::ByteArrayStatistics&>(*stats);
            const auto text = [](const parquet::ByteArray& b) {
                return std::string_view(reinterpret_cast<const char*>(b.ptr), b.len);
            };
            if (!keep(text(bytes.min()), text(bytes.max()))) {
                continue;
            }
        }
        groups.push_back(g);
    }
    std::shared_ptr<arrow::Table> table;
    if (const arrow::Status read = (*reader)->ReadRowGroups(groups, columns, &table);
        !read.ok() || !table) {
        return std::unexpected("cannot read the table: " + read.ToString());
    }
    // Each row group has a dictionary of its own: one for the whole table, its rows' places in it.
    auto unified = arrow::DictionaryUnifier::UnifyTable(*table, arrow::default_memory_pool());
    if (!unified.ok()) {
        return std::unexpected("cannot unify the dictionaries: " + unified.status().ToString());
    }
    auto combined = (*unified)->CombineChunks(arrow::default_memory_pool());
    if (!combined.ok()) {
        return std::unexpected("cannot combine chunks: " + combined.status().ToString());
    }
    return *combined;
}

/** The rows in time order, as written within a time. */
auto rows_in_time_order(const arrow::TimestampArray& times, std::vector<std::int64_t> rows)
    -> std::vector<std::int64_t> {
    const auto time_of = [&times](std::int64_t row) { return times.Value(row); };
    if (!std::ranges::is_sorted(rows, {}, time_of)) {
        std::ranges::stable_sort(rows, {}, time_of);
    }
    return rows;
}

auto to_upper(std::string text) -> std::string {
    std::ranges::transform(text, text.begin(), [](unsigned char c) -> char {
        return static_cast<char>(std::toupper(c));
    });
    return text;
}

auto read_parquet_quotes_or_throw(const std::string& file) -> ParquetQuotes {
    auto quotes = read_parquet_quotes(file);
    if (!quotes) {
        throw std::runtime_error(std::format("cannot read {}: {}", file, quotes.error()));
    }
    return std::move(*quotes);
}

} // namespace

auto read_parquet_quotes(const std::string& path) -> std::expected<ParquetQuotes, std::string> {
    const auto table = read_table(path, kQuoteColumnNames);
    if (!table) {
        return std::unexpected(table.error());
    }
    const arrow::Table& t = **table;
    const auto* times = static_cast<const arrow::TimestampArray*>(column(t, kTimeColumn));
    const arrow::TimeUnit::type unit =
        std::static_pointer_cast<arrow::TimestampType>(times->type())->unit();
    const Symbols symbol(column(t, "symbol"));
    const arrow::Array* bid = column(t, "bid_px");
    const arrow::Array* ask = column(t, "ask_px");
    const arrow::Array* bid_size = column(t, "bid_sz");
    const arrow::Array* ask_size = column(t, "ask_sz");
    const arrow::Array* underlying_bid = column(t, "underlying_bid_px");
    const arrow::Array* underlying_ask = column(t, "underlying_ask_px");
    const std::int64_t n = t.num_rows();

    std::vector<std::int64_t> all(static_cast<std::size_t>(n));
    std::iota(all.begin(), all.end(), std::int64_t{0});
    const std::vector<std::int64_t> order = rows_in_time_order(*times, std::move(all));

    ParquetQuotes quotes;
    quotes.path = path;
    quotes.codes = symbol.used();
    const bool has_null = std::ranges::any_of(
        std::views::iota(std::int64_t{0}, n), [&](std::int64_t row) { return symbol.at(row) < 0; });
    if (has_null && (quotes.codes.empty() || !quotes.codes.front().empty())) {
        // a null symbol is the empty code, as it was read before
        quotes.codes.insert(quotes.codes.begin(), std::string{});
    }
    // A dictionary value's code: its place in the sorted codes.
    std::vector<std::uint32_t> code_of(static_cast<std::size_t>(symbol.size()), 0);
    for (std::int64_t i = 0; i < symbol.size(); ++i) {
        if (const auto at = std::ranges::lower_bound(quotes.codes, symbol.value(i));
            at != quotes.codes.end() && *at == symbol.value(i)) {
            code_of[static_cast<std::size_t>(i)] = static_cast<std::uint32_t>(at - quotes.codes.begin());
        }
    }

    const double nan = std::numeric_limits<double>::quiet_NaN();
    for (std::vector<double>* values : {&quotes.bid, &quotes.ask, &quotes.bid_size,
                                        &quotes.ask_size, &quotes.underlying_bid,
                                        &quotes.underlying_ask}) {
        values->reserve(order.size());
    }
    quotes.code.reserve(order.size());
    for (const std::int64_t row : order) {
        const model::DateTime time = to_datetime(times->Value(row), unit);
        if (quotes.minutes.empty() || quotes.minutes.back().time != time) {
            quotes.minutes.push_back({.time = time, .first = quotes.rows(), .end = quotes.rows()});
        }
        const std::int64_t i = symbol.at(row);
        quotes.code.push_back(i < 0 ? 0 : code_of[static_cast<std::size_t>(i)]);
        quotes.bid.push_back(price_at(bid, row, 0.0));
        quotes.ask.push_back(price_at(ask, row, 0.0));
        quotes.bid_size.push_back(size_at(bid_size, row));
        quotes.ask_size.push_back(size_at(ask_size, row));
        quotes.underlying_bid.push_back(price_at(underlying_bid, row, nan));
        quotes.underlying_ask.push_back(price_at(underlying_ask, row, nan));
        quotes.minutes.back().end = quotes.rows();
    }
    return quotes;
}

auto day_of_file(const std::string& file) -> std::optional<std::chrono::year_month_day> {
    const std::string stem = std::filesystem::path(file).stem().string();
    const auto digit = [](unsigned char c) { return std::isdigit(c) != 0; };
    if (stem.size() != 8 || !std::ranges::all_of(stem, digit)) {
        return std::nullopt;
    }
    const std::chrono::year_month_day day{
        std::chrono::year{std::stoi(stem.substr(0, 4))},
        std::chrono::month{static_cast<unsigned>(std::stoi(stem.substr(4, 2)))},
        std::chrono::day{static_cast<unsigned>(std::stoi(stem.substr(6, 2)))}};
    return day.ok() ? std::optional{day} : std::nullopt;
}

auto portfolio_of_file(const std::string& file) -> std::string {
    namespace fs = std::filesystem;
    const fs::path path(file);
    const std::string name = path.filename().string();
    const std::string stem = name.substr(0, name.find('.'));
    if (stem.starts_with("backtest_")) { // backtest_<PORTFOLIO>_<start>_<end>
        const std::size_t end = stem.find('_', 9);
        return end == std::string::npos ? "" : to_upper(stem.substr(9, end - 9));
    }
    const std::string folder = path.parent_path().filename().string(); // <PORTFOLIO>-YYYY-MM
    return to_upper(folder.substr(0, folder.find('-')));
}

auto portfolios_of_files(std::span<const std::string> files) -> std::vector<std::string> {
    std::vector<std::string> out;
    for (const std::string& file : files) {
        if (std::string name = portfolio_of_file(file); !std::ranges::contains(out, name)) {
            out.push_back(std::move(name));
        }
    }
    return out;
}

auto contracts_of_codes(const std::string& portfolio, std::span<const std::string> codes)
    -> std::vector<model::Contract> {
    const std::string underlying = model::underlying_of(portfolio);
    const model::PortfolioSpec* spec = model::find_portfolio(portfolio);

    std::vector<model::Contract> out;
    model::Contract under;
    under.symbol = underlying;
    under.name = underlying;
    // One that trades (SPY) is a stock; one hedged with another (SPX) an index.
    under.product = spec != nullptr && spec->hedge == spec->underlying ? model::Product::EQUITY
                                                                       : model::Product::INDEX;
    under.multiplier = 1.0;
    under.price_tick = 0.01;
    out.push_back(std::move(under));
    for (const std::string& code : codes) {
        const std::optional<model::OptionId> id = model::parse_occ_code(portfolio, code);
        if (!id) {
            continue;
        }
        model::Contract option;
        option.symbol = model::option_symbol(*id);
        option.name = code;
        option.product = model::Product::OPTION;
        option.multiplier = 100.0;
        option.price_tick = 0.01;
        option.trading_class = portfolio;
        option.option_strike = id->strike;
        option.option_type = id->type;
        option.option_expiry = model::option_expiry_time(id->expiry);
        option.option_underlying = underlying;
        out.push_back(std::move(option));
    }
    return out;
}

auto contracts_of_files(std::span<const std::string> files, std::string_view portfolio)
    -> std::vector<model::Contract> {
    const std::string name(portfolio);
    std::vector<std::string> codes;
    for (const std::string& file : files) {
        if (portfolio_of_file(file) == name) {
            std::ranges::move(read_parquet_quotes_or_throw(file).codes, std::back_inserter(codes));
        }
    }
    std::ranges::sort(codes);
    codes.erase(std::ranges::unique(codes).begin(), codes.end());
    return contracts_of_codes(name, codes);
}

auto time_range_of_files(std::span<const std::string> files)
    -> std::pair<model::DateTime, model::DateTime> {
    model::DateTime first = model::DateTime::max();
    model::DateTime last = model::DateTime::min();
    for (const std::string& file : files) {
        const ParquetQuotes quotes = read_parquet_quotes_or_throw(file);
        if (!quotes.minutes.empty()) {
            first = std::min(first, quotes.minutes.front().time);
            last = std::max(last, quotes.minutes.back().time);
        }
    }
    if (first > last) {
        throw std::runtime_error("the simulated days quote nothing");
    }
    return {first, last};
}

// ---- The stored day
namespace {

// A stored day is a file for the day (its contracts, minutes and underlying) and one for each chain
// loaded (its quotes and their pricing), each a Header, its names (text, contracts), then its runs,
// each at a multiple of kAlign. Native byte order: read where written. What is read is the day as
// it was before it was stored, bit for bit; a chain's file is read in place (StoredChain).
constexpr std::size_t kAlign = 64;
constexpr std::array<char, 8> kDayMagic{'O', 'P', 'T', 'D', 'A', 'Y', '\0', '\0'};
constexpr std::array<char, 8> kChainMagic{'O', 'P', 'T', 'C', 'H', 'A', 'I', 'N'};
/** Bump when what a day file holds, or how a file is read into one, changes: stored days are then
 * read again. */
constexpr std::uint32_t kDayFormat = 3; // 2: the risk-free curve; 3: the underlying, then the
                                        // options in the listing's order
/** Bump when what a chain file holds changes (a change of the pricing is pricing_fingerprint's to
 * notice). 2: every column doubles, read in place; no Greeks, priced again from the IV when read.
 */
constexpr std::uint32_t kChainFormat = 2;
// Beyond any day: a header asking for more is not one.
constexpr std::uint64_t kMaxMinutes = 1U << 16;
constexpr std::uint64_t kMaxColumns = 1U << 20;
constexpr std::uint64_t kMaxNames = 1U << 30;

struct Header {
    std::array<char, 8> magic{};
    std::uint32_t format = 0;
    std::uint32_t reserved = 0;
    std::uint64_t minutes = 0;
    std::uint64_t options = 0;
    std::uint64_t chains = 0;
    std::uint64_t expiries = 0;
    std::uint64_t names = 0; // bytes
};

auto aligned(std::size_t n) -> std::size_t { return (n + kAlign - 1) / kAlign * kAlign; }

/** Where each of a file's runs starts, one after another. */
class Runs {
  public:
    explicit Runs(std::size_t names) : end_(aligned(sizeof(Header) + names)) {}
    /** The next run, of `bytes`: where it starts. */
    std::size_t next(std::size_t bytes) {
        const std::size_t at = end_;
        end_ = aligned(end_ + bytes);
        return at;
    }
    [[nodiscard]] std::size_t size() const { return end_; }

  private:
    std::size_t end_;
};

struct DayRuns {
    std::size_t times, underlying_bid, underlying_ask, size;
};

auto day_runs(const Header& h) -> DayRuns {
    Runs runs(h.names);
    const std::size_t n = h.minutes;
    DayRuns r{};
    r.times = runs.next(n * sizeof(std::int64_t));
    r.underlying_bid = runs.next(n * sizeof(double));
    r.underlying_ask = runs.next(n * sizeof(double));
    r.size = runs.size();
    return r;
}

/** A chain's runs: where each column starts (a minute a row of its options), then its forward;
 * and the file's size. */
auto chain_runs(const Header& h, std::array<std::size_t, StoredChain::kColumns + 1>& starts)
    -> std::size_t {
    Runs runs(h.names);
    for (std::size_t c = 0; c < StoredChain::kColumns; ++c) {
        starts[c] = runs.next(h.minutes * h.options * sizeof(double));
    }
    starts[StoredChain::kColumns] = runs.next(h.minutes * sizeof(double));
    return runs.size();
}

/** The file's header, when it has one of this kind and format within bounds. */
auto header_of(std::span<const std::byte> file, const std::array<char, 8>& magic,
               std::uint32_t format) -> std::optional<Header> {
    Header h;
    if (file.size() < sizeof h) {
        return std::nullopt;
    }
    std::memcpy(&h, file.data(), sizeof h);
    if (h.magic != magic || h.format != format || h.minutes > kMaxMinutes ||
        h.options > kMaxColumns || h.chains > kMaxColumns || h.expiries > kMaxColumns ||
        h.names > kMaxNames || sizeof h + h.names > file.size()) {
        return std::nullopt;
    }
    return h;
}

/** A file's names, written. */
class Names {
  public:
    template <typename T>
        requires std::is_trivially_copyable_v<T>
    void put(const T& value) {
        const auto* at = reinterpret_cast<const std::byte*>(&value);
        bytes_.insert(bytes_.end(), at, at + sizeof value);
    }
    void put(std::string_view text) {
        put<std::uint64_t>(text.size());
        const auto* at = reinterpret_cast<const std::byte*>(text.data());
        bytes_.insert(bytes_.end(), at, at + text.size());
    }
    template <typename T> void put(const std::optional<T>& value) {
        put<std::uint8_t>(value.has_value() ? 1 : 0);
        if (value) {
            put(*value);
        }
    }
    [[nodiscard]] std::span<const std::byte> bytes() const { return bytes_; }

  private:
    std::vector<std::byte> bytes_;
};

/** A file's names, read; past their end, nothing (ok() false). */
class Fields {
  public:
    explicit Fields(std::span<const std::byte> bytes) : bytes_(bytes) {}
    template <typename T>
        requires std::is_trivially_copyable_v<T>
    T get() {
        T value{};
        if (bytes_.size() < sizeof value) {
            ok_ = false;
            return value;
        }
        std::memcpy(&value, bytes_.data(), sizeof value);
        bytes_ = bytes_.subspan(sizeof value);
        return value;
    }
    std::string get_string() {
        const auto n = get<std::uint64_t>();
        if (!ok_ || n > bytes_.size()) {
            ok_ = false;
            return {};
        }
        std::string text(reinterpret_cast<const char*>(bytes_.data()), n);
        bytes_ = bytes_.subspan(n);
        return text;
    }
    bool has() { return get<std::uint8_t>() != 0; }
    [[nodiscard]] bool ok() const { return ok_; }

  private:
    std::span<const std::byte> bytes_;
    bool ok_ = true;
};

void put_contract(Names& w, const model::Contract& c) {
    w.put(c.symbol);
    w.put(c.name);
    w.put<std::uint8_t>(static_cast<std::uint8_t>(c.product));
    w.put(c.multiplier);
    w.put(c.price_tick);
    w.put(c.min_quantity);
    w.put(c.con_id);
    w.put(c.trading_class);
    w.put(c.option_strike);
    w.put(c.option_underlying);
    w.put<std::uint8_t>(c.option_type ? 1 : 0);
    if (c.option_type) {
        w.put<std::uint8_t>(static_cast<std::uint8_t>(*c.option_type));
    }
    w.put(c.option_expiry);
}

auto find_contract(Fields& r) -> model::Contract {
    model::Contract c;
    c.symbol = r.get_string();
    c.name = r.get_string();
    c.product = static_cast<model::Product>(r.get<std::uint8_t>());
    c.multiplier = r.get<double>();
    c.price_tick = r.get<double>();
    c.min_quantity = r.get<double>();
    if (r.has()) {
        c.con_id = r.get<int>();
    }
    c.trading_class = r.get_string();
    if (r.has()) {
        c.option_strike = r.get<double>();
    }
    if (r.has()) {
        c.option_underlying = r.get_string();
    }
    if (r.has()) {
        c.option_type = static_cast<model::OptionType>(r.get<std::uint8_t>());
    }
    if (r.has()) {
        c.option_expiry = r.get<model::DateTime>();
    }
    return c;
}

/** The header and the names in place, the runs zeros to fill. */
auto file_with_header(const Header& header, const Names& names, std::size_t size)
    -> std::vector<std::byte> {
    std::vector<std::byte> bytes(size);
    std::memcpy(bytes.data(), &header, sizeof header);
    std::ranges::copy(names.bytes(), bytes.begin() + sizeof header);
    return bytes;
}

/** `count` values of T from the file at `at`, into `to`. */
template <typename T>
void read_run(std::span<const std::byte> file, std::size_t at, std::size_t count,
              std::vector<T>& to) {
    to.resize(count);
    if (count > 0) {
        std::memcpy(to.data(), file.data() + at, count * sizeof(T));
    }
}

/** `values` into the file at `at`. */
template <typename T>
void write_run(std::vector<std::byte>& file, std::size_t at, std::span<const T> values) {
    if (!values.empty()) {
        std::memcpy(file.data() + at, values.data(), values.size_bytes());
    }
}

/** A file mapped read only, and its size. */
struct Mapped {
    std::shared_ptr<std::byte> bytes; // unmapped with the last copy
    std::size_t size = 0;

    [[nodiscard]] std::span<const std::byte> span() const { return {bytes.get(), size}; }
};

/** The whole file mapped, its access time set to now (the cache's trimming lets go of the files
 * read longest ago); none when it cannot be read or is empty. */
auto map_file(const std::string& path) -> std::optional<Mapped> {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return std::nullopt;
    }
    struct stat st{};
    void* at = ::fstat(fd, &st) == 0 && st.st_size > 0
                   ? ::mmap(nullptr, static_cast<std::size_t>(st.st_size), PROT_READ, MAP_PRIVATE,
                            fd, 0)
                   : MAP_FAILED;
    ::close(fd);
    if (at == MAP_FAILED) {
        return std::nullopt;
    }
    const auto size = static_cast<std::size_t>(st.st_size);
    const std::array<timespec, 2> times{timespec{.tv_sec = 0, .tv_nsec = UTIME_NOW},
                                        timespec{.tv_sec = 0, .tv_nsec = UTIME_OMIT}};
    ::utimensat(AT_FDCWD, path.c_str(), times.data(), 0);
    return Mapped{.bytes = std::shared_ptr<std::byte>(static_cast<std::byte*>(at),
                                                      [size](std::byte* p) { ::munmap(p, size); }),
                  .size = size};
}

/** Through a file renamed into place, so no reader sees half; false (said on stderr) when it
 * cannot be: the run goes on, and the next one reads the day again. */
auto write_file(const std::string& path, std::span<const std::byte> bytes) -> bool {
    namespace fs = std::filesystem;
    const fs::path target(path);
    const fs::path temporary =
        target.string() + std::format(".{}.{:x}.tmp", static_cast<long>(::getpid()),
                                      std::hash<std::thread::id>{}(std::this_thread::get_id()));
    try {
        fs::create_directories(target.parent_path());
        {
            std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(bytes.data()),
                      static_cast<std::streamsize>(bytes.size()));
            if (!out) {
                throw std::runtime_error("cannot write " + temporary.string());
            }
        }
        fs::rename(temporary, target);
        return true;
    } catch (const std::exception& e) {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        std::println(stderr, "Day not stored: {}", e.what());
        return false;
    }
}


} // namespace

// ---- A day for HistoricalData

auto read_day_index(const std::string& path) -> std::expected<DayIndex, std::string> {
    static constexpr std::array<const char*, 4> kNames{kTimeColumn, "symbol", "underlying_bid_px",
                                                       "underlying_ask_px"};
    const auto table = read_table(path, kNames);
    if (!table) {
        return std::unexpected(table.error());
    }
    const arrow::Table& t = **table;
    const auto* times = static_cast<const arrow::TimestampArray*>(column(t, kTimeColumn));
    const arrow::TimeUnit::type unit =
        std::static_pointer_cast<arrow::TimestampType>(times->type())->unit();
    const Symbols symbol(column(t, "symbol"));
    const arrow::Array* underlying_bid = column(t, "underlying_bid_px");
    const arrow::Array* underlying_ask = column(t, "underlying_ask_px");
    const std::int64_t n = t.num_rows();
    DayIndex day;

    const std::vector<std::string> codes = symbol.used(); // a null one is no contract
    day.portfolio = portfolio_of_file(path);
    day.rates = rates_of(t);
    if (day.portfolio.empty()) {
        day.portfolio = "UNKNOWN";
    }
    std::vector<model::Contract> contracts = contracts_of_codes(day.portfolio, codes);
    day.underlying = std::move(contracts.front());
    contracts.erase(contracts.begin());
    day.listing = std::make_shared<const model::Listing>(std::move(contracts));
    day.rows = n;

    // Per minute: the underlying's last quote in it (in the file's order within a time), the rows
    // not sorted for it.
    struct Minute {
        std::int64_t time;
        std::int64_t bid_row = -1;
        std::int64_t ask_row = -1;
    };
    std::vector<Minute> minutes;
    std::unordered_map<std::int64_t, std::size_t> minute_of;
    std::size_t last = 0; // the row before's minute: by contract the next one follows it
    for (std::int64_t row = 0; row < n; ++row) {
        const std::int64_t time = times->Value(row);
        if (minutes.empty() || minutes[last].time != time) {
            if (last + 1 < minutes.size() && minutes[last + 1].time == time) {
                ++last;
            } else {
                const auto [at, added] = minute_of.try_emplace(time, minutes.size());
                if (added) {
                    minutes.push_back({.time = time});
                }
                last = at->second;
            }
        }
        Minute& minute = minutes[last];
        if (underlying_bid != nullptr && !underlying_bid->IsNull(row) &&
            !std::isnan(price_at(underlying_bid, row, 0.0))) {
            minute.bid_row = row;
        }
        if (underlying_ask != nullptr && !underlying_ask->IsNull(row) &&
            !std::isnan(price_at(underlying_ask, row, 0.0))) {
            minute.ask_row = row;
        }
    }
    std::ranges::sort(minutes, {}, &Minute::time);
    for (const Minute& minute : minutes) {
        day.times.push_back(to_datetime(minute.time, unit).time_since_epoch().count());
        day.underlying_bid.push_back(
            minute.bid_row < 0 ? 0.0 : price_at(underlying_bid, minute.bid_row, 0.0));
        day.underlying_ask.push_back(
            minute.ask_row < 0 ? 0.0 : price_at(underlying_ask, minute.ask_row, 0.0));
    }
    if (!day.times.empty()) {
        day.day = std::chrono::floor<std::chrono::days>(
            model::DateTime{model::DateTime::duration{day.times.front()}});
    }
    return day;
}

auto read_chain_quotes(const std::string& path, std::span<const std::int64_t> times,
                       std::span<const model::Contract> options, StoredChain& stored)
    -> std::expected<void, std::string> {
    if (options.empty() || times.empty()) {
        return {};
    }
    static constexpr std::array<const char*, 6> kNames{kTimeColumn, "symbol", "bid_px",
                                                       "ask_px",    "bid_sz", "ask_sz"};
    const std::string_view expiry = std::string_view(options.front().name).substr(0, 6);
    const auto table = read_table(path, kNames, [expiry](std::string_view first,
                                                         std::string_view last) {
        return first.substr(0, 6) <= expiry && expiry <= last.substr(0, 6);
    });
    if (!table) {
        return std::unexpected(table.error());
    }
    const arrow::Table& t = **table;
    const auto* time = static_cast<const arrow::TimestampArray*>(column(t, kTimeColumn));
    const arrow::TimeUnit::type unit =
        std::static_pointer_cast<arrow::TimestampType>(time->type())->unit();
    const Symbols symbol(column(t, "symbol"));
    const arrow::Array* bid = column(t, "bid_px");
    const arrow::Array* ask = column(t, "ask_px");
    const arrow::Array* bid_size = column(t, "bid_sz");
    const arrow::Array* ask_size = column(t, "ask_sz");
    std::unordered_map<std::string_view, std::uint32_t> option_of; // a code's place in the chain
    for (std::size_t k = 0; k < options.size(); ++k) {
        option_of.emplace(options[k].name, static_cast<std::uint32_t>(k));
    }
    constexpr std::int64_t kNotInChain = -1;
    std::vector<std::int64_t> option_of_value(static_cast<std::size_t>(symbol.size()), kNotInChain);
    for (std::int64_t i = 0; i < symbol.size(); ++i) {
        if (const auto at = option_of.find(symbol.value(i)); at != option_of.end()) {
            option_of_value[static_cast<std::size_t>(i)] = at->second;
        }
    }
    std::vector<std::int64_t> kept;
    std::vector<std::uint32_t> option(static_cast<std::size_t>(t.num_rows()), 0);
    for (std::int64_t row = 0; row < t.num_rows(); ++row) {
        const std::int64_t i = symbol.at(row);
        if (const std::int64_t k = i < 0 ? kNotInChain : option_of_value[static_cast<std::size_t>(i)];
            k != kNotInChain) {
            kept.push_back(row);
            option[static_cast<std::size_t>(row)] = static_cast<std::uint32_t>(k);
        }
    }
    // Rows in time order: the minutes before a row's carry their quotes on to it first.
    constexpr std::size_t kQuotes = 4; // bid, ask, bid_size, ask_size: StoredChain's first four
    std::size_t carried = 0;           // minutes [0, carried] hold their quotes
    const auto carry_to = [&](std::size_t minute) {
        for (; carried < minute; ++carried) {
            for (std::size_t q = 0; q < kQuotes; ++q) {
                std::ranges::copy(stored.row(q, carried), stored.row(q, carried + 1).begin());
            }
        }
    };
    for (const std::int64_t row : rows_in_time_order(*time, std::move(kept))) {
        const std::int64_t at = to_datetime(time->Value(row), unit).time_since_epoch().count();
        const auto minute = std::ranges::lower_bound(times, at);
        if (minute == times.end() || *minute != at) {
            continue;
        }
        const auto i = static_cast<std::size_t>(minute - times.begin());
        carry_to(i);
        const std::uint32_t k = option[static_cast<std::size_t>(row)];
        stored.row(0, i)[k] = price_at(bid, row, 0.0);
        stored.row(1, i)[k] = price_at(ask, row, 0.0);
        stored.row(2, i)[k] = size_at(bid_size, row);
        stored.row(3, i)[k] = size_at(ask_size, row);
    }
    carry_to(times.size() - 1);
    return {};
}

namespace {

auto encode_day(const DayIndex& day) -> std::vector<std::byte> {
    Names names;
    names.put(day.portfolio);
    names.put(day.day);
    names.put(day.rows);
    names.put<std::uint64_t>(day.listing->size());
    put_contract(names, day.underlying);
    for (const model::Contract& contract : day.listing->contracts()) {
        put_contract(names, contract);
    }
    names.put<std::uint64_t>(day.rates ? day.rates->years.size() : 0);
    if (day.rates) {
        for (std::size_t j = 0; j < day.rates->years.size(); ++j) {
            names.put(day.rates->years[j]);
            names.put(day.rates->rates[j]);
        }
    }
    const Header header{.magic = kDayMagic,
                        .format = kDayFormat,
                        .minutes = day.times.size(),
                        .names = names.bytes().size()};
    const DayRuns runs = day_runs(header);
    std::vector<std::byte> file = file_with_header(header, names, runs.size);
    write_run(file, runs.times, std::span(day.times));
    write_run(file, runs.underlying_bid, std::span(day.underlying_bid));
    write_run(file, runs.underlying_ask, std::span(day.underlying_ask));
    return file;
}

auto decode_day(std::span<const std::byte> file) -> std::optional<DayIndex> {
    const std::optional<Header> h = header_of(file, kDayMagic, kDayFormat);
    if (!h || h->options != 0 || h->chains != 0 || h->expiries != 0) {
        return std::nullopt;
    }
    Fields f(file.subspan(sizeof(Header), h->names));
    DayIndex day;
    day.portfolio = f.get_string();
    if (f.has()) {
        day.day = f.get<model::DateTime>();
    }
    day.rows = f.get<std::int64_t>();
    const auto n_options = f.get<std::uint64_t>();
    if (!f.ok() || n_options > kMaxColumns) {
        return std::nullopt;
    }
    day.underlying = find_contract(f);
    std::vector<model::Contract> options; // in the listing's order: listed in one pass
    options.reserve(n_options);
    for (std::uint64_t i = 0; i < n_options && f.ok(); ++i) {
        options.push_back(find_contract(f));
    }
    const auto n_rates = f.get<std::uint64_t>();
    if (!f.ok() || n_rates > kMaxColumns) {
        return std::nullopt;
    }
    if (n_rates > 0) {
        day.rates = model::RateCurve{.years = {}, .rates = {}};
        for (std::uint64_t j = 0; j < n_rates && f.ok(); ++j) {
            day.rates->years.push_back(f.get<double>());
            day.rates->rates.push_back(f.get<double>());
        }
    }
    const DayRuns runs = day_runs(*h);
    if (!f.ok() || runs.size != file.size()) {
        return std::nullopt;
    }
    const std::size_t n = h->minutes;
    read_run(file, runs.times, n, day.times);
    read_run(file, runs.underlying_bid, n, day.underlying_bid);
    read_run(file, runs.underlying_ask, n, day.underlying_ask);
    day.listing = std::make_shared<const model::Listing>(std::move(options));
    return day;
}

/** A chain file's names: the chain, then its options' symbols (the columns, to check). */
auto chain_names(std::string_view chain, std::span<const model::Contract> options) -> Names {
    Names names;
    names.put(chain);
    for (const model::Contract& option : options) {
        names.put(option.symbol);
    }
    return names;
}

} // namespace

auto read_stored_day(const std::string& file) -> std::optional<DayIndex> {
    const std::optional<Mapped> mapped = map_file(file);
    return mapped ? decode_day(mapped->span()) : std::nullopt;
}

auto store_day(const std::string& file, const DayIndex& day) -> bool {
    return write_file(file, encode_day(day));
}

StoredChain::StoredChain(std::string_view chain, std::span<const model::Contract> options,
                         std::size_t minutes)
    : options_(options.size()) {
    const Names names = chain_names(chain, options);
    const Header header{.magic = kChainMagic,
                        .format = kChainFormat,
                        .minutes = minutes,
                        .options = options.size(),
                        .chains = 1,
                        .names = names.bytes().size()};
    size_ = chain_runs(header, runs_);
    bytes_ = std::shared_ptr<std::byte>(new std::byte[size_](), std::default_delete<std::byte[]>());
    std::memcpy(bytes_.get(), &header, sizeof header);
    std::ranges::copy(names.bytes(), bytes_.get() + sizeof header);
}

auto StoredChain::read(const std::string& file, std::string_view chain,
                       std::span<const model::Contract> options, std::size_t minutes)
    -> std::optional<StoredChain> {
    const std::optional<Mapped> mapped = map_file(file);
    if (!mapped) {
        return std::nullopt;
    }
    const std::optional<Header> h = header_of(mapped->span(), kChainMagic, kChainFormat);
    if (!h || h->minutes != minutes || h->options != options.size() || h->chains != 1 ||
        h->expiries != 0) {
        return std::nullopt;
    }
    const Names names = chain_names(chain, options);
    if (h->names != names.bytes().size() ||
        !std::ranges::equal(names.bytes(), mapped->span().subspan(sizeof(Header), h->names))) {
        return std::nullopt;
    }
    StoredChain stored;
    stored.options_ = options.size();
    stored.size_ = chain_runs(*h, stored.runs_);
    if (stored.size_ != mapped->size) {
        return std::nullopt;
    }
    stored.bytes_ = mapped->bytes;
    return stored;
}

auto StoredChain::store(const std::string& file) const -> bool {
    return write_file(file, {bytes_.get(), size_});
}

namespace {

/** A day file's key: its root, name, size and time written, and the format. */
auto day_key(const std::string& parquet_path, std::string& root, std::string& stem)
    -> utilities::Fnv {
    namespace fs = std::filesystem;
    const fs::path file(parquet_path);
    std::error_code error;
    const std::uintmax_t size = fs::file_size(file, error);
    const auto written = fs::last_write_time(file, error).time_since_epoch().count();
    root = portfolio_of_file(parquet_path);
    if (root.empty()) {
        root = "UNKNOWN";
    }
    stem = file.stem().string();
    utilities::Fnv key;
    key.add(kDayMagic);
    key.add(kDayFormat);
    key.add(root);
    key.add(stem);
    key.add(size);
    key.add(written);
    return key;
}

} // namespace

auto stored_day(const std::string& cache_dir, const std::string& parquet_path) -> std::string {
    std::string root;
    std::string stem;
    const utilities::Fnv key = day_key(parquet_path, root, stem);
    return (std::filesystem::path(cache_dir) / root /
            std::format("{}-{:016x}.day", stem, key.value()))
        .string();
}

auto stored_chain(const std::string& cache_dir, const std::string& parquet_path,
                  std::string_view chain, std::uint64_t pricing, double risk_free_rate,
                  std::string_view iv_price_mode) -> std::string {
    std::string root;
    std::string stem;
    utilities::Fnv key = day_key(parquet_path, root, stem);
    key.add(kChainMagic);
    key.add(kChainFormat);
    key.add(chain);
    key.add(pricing);
    key.add(risk_free_rate);
    key.add(iv_price_mode);
    return (std::filesystem::path(cache_dir) / root /
            std::format("{}-{}-{:016x}.chain", stem, chain, key.value()))
        .string();
}

auto day_stored(const std::string& cache_dir, const std::string& parquet_path) -> bool {
    std::error_code error;
    return std::filesystem::exists(stored_day(cache_dir, parquet_path), error);
}

} // namespace adapters
