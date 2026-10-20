#include "research/offline_data.hpp"

#include <algorithm>
#include <array>
#include <arrow/api.h>
#include <arrow/array/concatenate.h>
#include <arrow/io/api.h>
#include <arrow/util/key_value_metadata.h>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <memory>
#include <numeric>
#include <parquet/arrow/reader.h>
#include <parquet/arrow/writer.h>
#include <parquet/file_reader.h>
#include <parquet/metadata.h>

namespace research {

namespace {

/** A Parquet file opened for reading a row group and a column at a time. */
class ParquetFile {
  public:
    /** dictionary: a string column read by its dictionary (each value once, a row its index). */
    static std::expected<ParquetFile, std::string> open(const std::string& path,
                                                        const std::string& dictionary = {});

    int row_groups() const;
    /** The column's index; -1 when the file has none by that name. */
    int column(const std::string& name) const;
    /** A value of the file's key-value metadata; empty when it has none. */
    std::string metadata(const std::string& key) const;
    std::expected<std::shared_ptr<arrow::Table>, std::string>
    read(int row_group, const std::vector<int>& columns) const;
    std::expected<std::shared_ptr<arrow::Table>, std::string>
    read_all(const std::vector<int>& columns) const;

  private:
    std::shared_ptr<parquet::arrow::FileReader> reader_;
    std::shared_ptr<arrow::Schema> schema_;
};

auto ParquetFile::open(const std::string& path, const std::string& dictionary)
    -> std::expected<ParquetFile, std::string> {
    auto opened = arrow::io::MemoryMappedFile::Open(path, arrow::io::FileMode::READ);
    if (!opened.ok()) {
        return std::unexpected("cannot open " + path + ": " + opened.status().ToString());
    }
    parquet::arrow::FileReaderBuilder builder;
    if (const arrow::Status open = builder.Open(*opened); !open.ok()) {
        return std::unexpected(path + " is not a Parquet file: " + open.ToString());
    }
    parquet::ArrowReaderProperties properties = parquet::default_arrow_reader_properties();
    if (const int c = builder.raw_reader()->metadata()->schema()->ColumnIndex(dictionary);
        !dictionary.empty() && c >= 0) {
        properties.set_read_dictionary(c, true);
    }
    auto reader = builder.properties(properties)->Build();
    if (!reader.ok()) {
        return std::unexpected(path + ": " + reader.status().ToString());
    }
    ParquetFile file;
    file.reader_ = std::move(*reader);
    if (const arrow::Status schema = file.reader_->GetSchema(&file.schema_); !schema.ok()) {
        return std::unexpected(path + ": cannot read the schema: " + schema.ToString());
    }
    return file;
}

auto ParquetFile::row_groups() const -> int { return reader_->num_row_groups(); }

auto ParquetFile::column(const std::string& name) const -> int {
    return schema_->GetFieldIndex(name);
}

auto ParquetFile::metadata(const std::string& key) const -> std::string {
    const std::shared_ptr<const arrow::KeyValueMetadata>& meta = schema_->metadata();
    if (meta == nullptr) {
        return {};
    }
    const int i = meta->FindKey(key);
    return i < 0 ? std::string() : meta->value(i);
}

auto ParquetFile::read(int row_group, const std::vector<int>& columns) const
    -> std::expected<std::shared_ptr<arrow::Table>, std::string> {
    std::shared_ptr<arrow::Table> table;
    if (const arrow::Status read = reader_->ReadRowGroup(row_group, columns, &table);
        !read.ok() || !table) {
        return std::unexpected("cannot read row group " + std::to_string(row_group) + ": " +
                               read.ToString());
    }
    return table;
}

auto ParquetFile::read_all(const std::vector<int>& columns) const
    -> std::expected<std::shared_ptr<arrow::Table>, std::string> {
    std::shared_ptr<arrow::Table> table;
    if (const arrow::Status read = reader_->ReadTable(columns, &table); !read.ok() || !table) {
        return std::unexpected("cannot read the table: " + read.ToString());
    }
    return table;
}

auto column_of(const arrow::Table& table, int i) -> std::shared_ptr<arrow::Array> {
    const std::shared_ptr<arrow::ChunkedArray>& column = table.column(i);
    if (column->num_chunks() == 1) {
        return column->chunk(0);
    }
    auto combined = arrow::Concatenate(column->chunks());
    return combined.ok() ? *combined : nullptr;
}

/** In time order: a few ticks come stamped before the one ahead of them. */
void sort_by_time(QuoteSeries& s) {
    if (std::ranges::is_sorted(s.ms)) {
        return;
    }
    std::vector<std::size_t> order(s.ms.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::ranges::stable_sort(order, {}, [&](std::size_t i) { return s.ms[i]; });
    QuoteSeries sorted;
    sorted.ms.reserve(order.size());
    sorted.bid.reserve(order.size());
    sorted.ask.reserve(order.size());
    for (const std::size_t i : order) {
        sorted.ms.push_back(s.ms[i]);
        sorted.bid.push_back(s.bid[i]);
        sorted.ask.push_back(s.ask[i]);
    }
    s = std::move(sorted);
}

} // namespace

auto for_each_contract(const std::string& path, const std::function<bool(double)>& keep,
                       const std::function<void(const Contract&, QuoteSeries&&)>& visit)
    -> std::expected<void, std::string> {
    auto file = ParquetFile::open(path);
    if (!file) {
        return std::unexpected(file.error());
    }
    const std::string places_text = file->metadata("price_places");
    if (places_text.empty()) {
        return std::unexpected(path + ": no price_places in its metadata");
    }
    const double scale = std::pow(10.0, std::stoi(places_text));
    const int strike_col = file->column("strike");
    const std::vector<int> columns{file->column("call"), file->column("ms"), file->column("bid"),
                                   file->column("ask")};
    if (strike_col < 0 || std::ranges::contains(columns, -1)) {
        return std::unexpected(path + ": not a ticks file (strike, call, ms, bid, ask)");
    }
    std::map<Contract, QuoteSeries> pending;
    const auto finish = [&](auto it) {
        sort_by_time(it->second);
        visit(it->first, std::move(it->second));
        return pending.erase(it);
    };
    for (int g = 0; g < file->row_groups(); ++g) {
        auto strikes = file->read(g, {strike_col});
        if (!strikes) {
            return std::unexpected(path + ": " + strikes.error());
        }
        const auto strike_array = std::static_pointer_cast<arrow::DoubleArray>(
            column_of(**strikes, 0));
        const std::int64_t n = strike_array->length();
        std::vector<char> wanted(static_cast<std::size_t>(n));
        bool any = false;
        double last_strike = std::nan("");
        bool last_kept = false;
        for (std::int64_t i = 0; i < n; ++i) {
            const double k = strike_array->Value(i);
            if (k != last_strike) {
                last_strike = k;
                last_kept = keep(k);
            }
            wanted[static_cast<std::size_t>(i)] = last_kept ? 1 : 0;
            any = any || last_kept;
        }
        if (!any) {
            while (!pending.empty()) {
                finish(pending.begin());
            }
            continue;
        }
        auto rows = file->read(g, columns);
        if (!rows) {
            return std::unexpected(path + ": " + rows.error());
        }
        const auto call = std::static_pointer_cast<arrow::Int8Array>(column_of(**rows, 0));
        const auto ms = std::static_pointer_cast<arrow::Int64Array>(column_of(**rows, 1));
        const auto bid = std::static_pointer_cast<arrow::Int64Array>(column_of(**rows, 2));
        const auto ask = std::static_pointer_cast<arrow::Int64Array>(column_of(**rows, 3));
        double bid_now = 0.0;
        double ask_now = 0.0;
        QuoteSeries* into = nullptr;
        Contract into_key{.strike = std::nan(""), .call = false};
        for (std::int64_t i = 0; i < n; ++i) {
            if (bid->IsValid(i)) { // a contract's first row always is: its nulls are its own
                bid_now = static_cast<double>(bid->Value(i)) / scale;
            }
            if (ask->IsValid(i)) {
                ask_now = static_cast<double>(ask->Value(i)) / scale;
            }
            if (wanted[static_cast<std::size_t>(i)] == 0) {
                continue;
            }
            const Contract key{.strike = strike_array->Value(i), .call = call->Value(i) != 0};
            if (into == nullptr || key != into_key) {
                into = &pending[key];
                into_key = key;
            }
            into->ms.push_back(ms->Value(i));
            into->bid.push_back(bid_now);
            into->ask.push_back(ask_now);
        }
        // A contract not running on into the next row group is done.
        const Contract last{.strike = strike_array->Value(n - 1), .call = call->Value(n - 1) != 0};
        for (auto it = pending.begin(); it != pending.end();) {
            it = it->first == last ? std::next(it) : finish(it);
        }
    }
    while (!pending.empty()) {
        finish(pending.begin());
    }
    return {};
}

auto read_trades(const std::string& path, int expiry, std::int64_t day_start_ms)
    -> std::expected<std::map<Contract, TradeSeries>, std::string> {
    auto file = ParquetFile::open(path);
    if (!file) {
        return std::unexpected(file.error());
    }
    const std::string places_text = file->metadata("price_places");
    if (places_text.empty()) {
        return std::unexpected(path + ": no price_places in its metadata");
    }
    const double scale = std::pow(10.0, std::stoi(places_text));
    const std::vector<int> columns{file->column("expiry"), file->column("strike"),
                                   file->column("call"), file->column("trade_ms"),
                                   file->column("price")};
    if (std::ranges::contains(columns, -1)) {
        return std::unexpected(path +
                               ": not a trades file (expiry, strike, call, trade_ms, price)");
    }
    std::map<Contract, TradeSeries> out;
    for (int g = 0; g < file->row_groups(); ++g) {
        auto table = file->read(g, columns);
        if (!table) {
            return std::unexpected(path + ": " + table.error());
        }
        for (int c = 0; c < (*table)->column(0)->num_chunks(); ++c) {
            const auto& e = static_cast<const arrow::Int32Array&>(*(*table)->column(0)->chunk(c));
            const auto& k = static_cast<const arrow::Int32Array&>(*(*table)->column(1)->chunk(c));
            const auto& call = static_cast<const arrow::Int8Array&>(*(*table)->column(2)->chunk(c));
            const auto& t = static_cast<const arrow::Int32Array&>(*(*table)->column(3)->chunk(c));
            const auto& p = static_cast<const arrow::Int64Array&>(*(*table)->column(4)->chunk(c));
            for (std::int64_t i = 0; i < e.length(); ++i) {
                if (e.Value(i) != expiry) {
                    continue;
                }
                TradeSeries& s = out[Contract{.strike = k.Value(i) / 1000.0,
                                              .call = call.Value(i) != 0}];
                s.ms.push_back(day_start_ms + t.Value(i));
                s.price.push_back(static_cast<double>(p.Value(i)) / scale);
            }
        }
    }
    for (auto& [contract, s] : out) {
        if (std::ranges::is_sorted(s.ms)) {
            continue;
        }
        std::vector<std::size_t> order(s.ms.size());
        std::iota(order.begin(), order.end(), std::size_t{0});
        std::ranges::stable_sort(order, {}, [&](std::size_t i) { return s.ms[i]; });
        TradeSeries sorted;
        for (const std::size_t i : order) {
            sorted.ms.push_back(s.ms[i]);
            sorted.price.push_back(s.price[i]);
        }
        s = std::move(sorted);
    }
    return out;
}

auto read_trade_rows(const std::string& path, std::span<const std::int16_t> conditions)
    -> std::expected<TradeRows, std::string> {
    auto file = ParquetFile::open(path);
    if (!file) {
        return std::unexpected(file.error());
    }
    const std::string places_text = file->metadata("price_places");
    if (places_text.empty()) {
        return std::unexpected(path + ": no price_places in its metadata");
    }
    const double scale = std::pow(10.0, -std::stoi(places_text));
    const std::vector<int> columns{file->column("expiry"),    file->column("strike"),
                                   file->column("call"),      file->column("trade_ms"),
                                   file->column("condition"), file->column("exchange"),
                                   file->column("price"),     file->column("bid"),
                                   file->column("ask"),       file->column("size")};
    if (std::ranges::contains(columns, -1)) {
        return std::unexpected(path + ": not a trades file (expiry, strike, call, trade_ms, "
                                      "condition, exchange, price, bid, ask, size)");
    }
    std::array<bool, 1 << 15> wanted{}; // by condition
    wanted.fill(conditions.empty());
    for (const std::int16_t c : conditions) {
        if (c >= 0) {
            wanted[static_cast<std::size_t>(c)] = true;
        }
    }
    const auto dollars = [scale](const arrow::Int64Array& a, std::int64_t i) {
        return a.IsNull(i) ? std::nan("") : static_cast<double>(a.Value(i)) * scale;
    };
    TradeRows out;
    for (int g = 0; g < file->row_groups(); ++g) {
        auto table = file->read(g, columns);
        if (!table) {
            return std::unexpected(path + ": " + table.error());
        }
        const auto col = [&](int i) { return column_of(**table, i); };
        const auto expiry = std::static_pointer_cast<arrow::Int32Array>(col(0));
        const auto strike = std::static_pointer_cast<arrow::Int32Array>(col(1));
        const auto call = std::static_pointer_cast<arrow::Int8Array>(col(2));
        const auto ms = std::static_pointer_cast<arrow::Int32Array>(col(3));
        const auto condition = std::static_pointer_cast<arrow::Int16Array>(col(4));
        const auto exchange = std::static_pointer_cast<arrow::Int16Array>(col(5));
        const auto price = std::static_pointer_cast<arrow::Int64Array>(col(6));
        const auto bid = std::static_pointer_cast<arrow::Int64Array>(col(7));
        const auto ask = std::static_pointer_cast<arrow::Int64Array>(col(8));
        const auto size = std::static_pointer_cast<arrow::Int32Array>(col(9));
        const std::int64_t n = ms->length();
        for (std::int64_t i = 0; i < n; ++i) {
            const std::int16_t c = condition->Value(i);
            if (c < 0 || !wanted[static_cast<std::size_t>(c)]) {
                continue;
            }
            out.expiry.push_back(expiry->Value(i));
            out.strike.push_back(strike->Value(i) / 1000.0);
            out.call.push_back(call->Value(i));
            out.ms.push_back(ms->Value(i));
            out.condition.push_back(c);
            out.exchange.push_back(exchange->Value(i));
            out.price.push_back(dollars(*price, i));
            out.bid.push_back(dollars(*bid, i));
            out.ask.push_back(dollars(*ask, i));
            out.size.push_back(size->IsNull(i) ? 0 : size->Value(i));
        }
    }
    return out;
}

auto read_open_interest(const std::string& path)
    -> std::expected<std::unordered_map<std::string, double>, std::string> {
    auto file = ParquetFile::open(path, "symbol");
    if (!file) {
        return std::unexpected(file.error());
    }
    const std::vector<int> columns{file->column("symbol"), file->column("open_interest")};
    if (std::ranges::contains(columns, -1)) {
        return std::unexpected(path + ": no symbol or open_interest column");
    }
    std::unordered_map<std::string, double> out;
    for (int g = 0; g < file->row_groups(); ++g) {
        auto table = file->read(g, columns);
        if (!table) {
            return std::unexpected(path + ": " + table.error());
        }
        const auto symbols =
            std::static_pointer_cast<arrow::DictionaryArray>(column_of(**table, 0));
        const auto oi = std::static_pointer_cast<arrow::Int64Array>(column_of(**table, 1));
        const auto& names = static_cast<const arrow::StringArray&>(*symbols->dictionary());
        std::vector<char> seen(static_cast<std::size_t>(names.length()), 0);
        for (std::int64_t i = 0; i < symbols->length(); ++i) {
            if (symbols->IsNull(i) || oi->IsNull(i)) {
                continue;
            }
            const auto k = static_cast<std::size_t>(symbols->GetValueIndex(i));
            if (seen[k] == 0) { // a contract's value is the day's: its first row's
                seen[k] = 1;
                out.try_emplace(std::string(names.GetView(static_cast<std::int64_t>(k))),
                                static_cast<double>(oi->Value(i)));
            }
        }
    }
    return out;
}

auto read_contract_nets(const std::string& path)
    -> std::expected<std::vector<ContractNet>, std::string> {
    auto file = ParquetFile::open(path);
    if (!file) {
        return std::unexpected(file.error());
    }
    const std::vector<int> columns{file->column("day"), file->column("expiry"),
                                   file->column("strike_milli"), file->column("call"),
                                   file->column("net"), file->column("volume")};
    if (std::ranges::contains(columns, -1)) {
        return std::unexpected(path + ": not a table of contracts' nets");
    }
    auto table = file->read_all(columns);
    if (!table) {
        return std::unexpected(path + ": " + table.error());
    }
    const auto int32s = [&](int i) {
        return std::static_pointer_cast<arrow::Int32Array>(column_of(**table, i));
    };
    const auto day = int32s(0);
    const auto expiry = int32s(1);
    const auto strike = int32s(2);
    const auto call = std::static_pointer_cast<arrow::Int8Array>(column_of(**table, 3));
    const auto net = int32s(4);
    const auto volume = int32s(5);
    std::vector<ContractNet> out(static_cast<std::size_t>(day->length()));
    for (std::int64_t i = 0; i < day->length(); ++i) {
        out[static_cast<std::size_t>(i)] = {.day = day->Value(i), .expiry = expiry->Value(i),
                                            .strike_milli = strike->Value(i),
                                            .call = call->Value(i) != 0, .net = net->Value(i),
                                            .volume = volume->Value(i)};
    }
    return out;
}

namespace {

template <typename Builder, typename T>
auto build(const std::vector<T>& values) -> std::shared_ptr<arrow::Array> {
    Builder builder;
    (void)builder.AppendValues(values);
    std::shared_ptr<arrow::Array> out;
    (void)builder.Finish(&out);
    return out;
}

template <typename Builder, typename T>
auto build_lists(const std::vector<std::vector<T>>& values) -> std::shared_ptr<arrow::Array> {
    auto items = std::make_shared<Builder>();
    arrow::ListBuilder builder(arrow::default_memory_pool(), items);
    for (const std::vector<T>& v : values) {
        (void)builder.Append();
        (void)items->AppendValues(v);
    }
    std::shared_ptr<arrow::Array> out;
    (void)builder.Finish(&out);
    return out;
}

} // namespace

auto column(const std::vector<std::int8_t>& v) -> std::shared_ptr<arrow::Array> {
    return build<arrow::Int8Builder>(v);
}
auto column(const std::vector<int>& v) -> std::shared_ptr<arrow::Array> {
    return build<arrow::Int32Builder>(v);
}
auto column(const std::vector<std::int64_t>& v) -> std::shared_ptr<arrow::Array> {
    return build<arrow::Int64Builder>(v);
}
auto column(const std::vector<double>& v) -> std::shared_ptr<arrow::Array> {
    return build<arrow::DoubleBuilder>(v);
}
auto column(const std::vector<bool>& v) -> std::shared_ptr<arrow::Array> {
    return build<arrow::BooleanBuilder>(v);
}
auto column(const std::vector<std::string>& v) -> std::shared_ptr<arrow::Array> {
    return build<arrow::StringBuilder>(v);
}
auto list_column(const std::vector<std::vector<double>>& v) -> std::shared_ptr<arrow::Array> {
    return build_lists<arrow::DoubleBuilder>(v);
}
auto list_column(const std::vector<std::vector<int>>& v) -> std::shared_ptr<arrow::Array> {
    return build_lists<arrow::Int32Builder>(v);
}
auto list_column(const std::vector<std::vector<bool>>& v) -> std::shared_ptr<arrow::Array> {
    return build_lists<arrow::BooleanBuilder>(v);
}

auto write_table(const std::string& path, const std::shared_ptr<arrow::Table>& table)
    -> std::expected<void, std::string> {
    auto out = arrow::io::FileOutputStream::Open(path);
    if (!out.ok()) {
        return std::unexpected("cannot write " + path + ": " + out.status().ToString());
    }
    const arrow::Status written =
        parquet::arrow::WriteTable(*table, arrow::default_memory_pool(), *out, 1 << 20);
    if (!written.ok()) {
        return std::unexpected("cannot write " + path + ": " + written.ToString());
    }
    return {};
}

auto parse_day(const std::string& text) -> std::chrono::year_month_day {
    std::istringstream in(text);
    std::chrono::year_month_day day{};
    in >> std::chrono::parse("%F", day);
    if (in.fail() || !day.ok()) {
        throw std::invalid_argument("not a date (YYYY-MM-DD): " + text);
    }
    return day;
}

auto Holdouts::load() -> Holdouts {
    const char* home = std::getenv("OPTRA_HOME");
    const char* user = std::getenv("HOME");
    const std::filesystem::path config =
        home != nullptr
            ? std::filesystem::path(home) / "config.json"
            : std::filesystem::path(user != nullptr ? user : "") / ".optra" / "config.json";
    Holdouts out;
    std::ifstream in(config);
    if (!in) {
        return out;
    }
    const nlohmann::json json = nlohmann::json::parse(in, nullptr, false);
    if (!json.is_object() || !json.contains("holdout") || !json.at("holdout").is_object()) {
        return out;
    }
    for (const auto& [portfolio, day] : json.at("holdout").items()) {
        const int from = yyyymmdd(parse_day(day.get<std::string>()));
        out.by_portfolio_.emplace(portfolio, from);
        out.earliest_ = out.earliest_ == 0 ? from : std::min(out.earliest_, from);
    }
    return out;
}

auto Holdouts::of(std::string_view portfolio) const -> int {
    const auto it = by_portfolio_.find(portfolio);
    return it != by_portfolio_.end() ? it->second : earliest_;
}

auto Holdouts::first_held(std::span<const std::string> files) const -> std::optional<std::string> {
    for (const std::string& file : files) {
        const std::filesystem::path path(file);
        const std::string name = path.stem().string();
        if (name.size() < 8 || !std::all_of(name.begin(), name.begin() + 8, [](unsigned char ch) {
                return std::isdigit(ch) != 0;
            })) {
            continue; // not a day's file
        }
        const int from = of(path.parent_path().parent_path().filename().string());
        if (from != 0 && std::stoi(name.substr(0, 8)) >= from) {
            return file;
        }
    }
    return std::nullopt;
}

} // namespace research
