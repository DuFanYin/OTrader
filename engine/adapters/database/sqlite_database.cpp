#include "adapters/database/sqlite_database.hpp"
#include "model/symbol.hpp"
#include "model/utility.hpp"
#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <sqlite3.h>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace adapters {

using model::datetime_to_str;
using core::SavedRecord;
using model::StrategyRecord;
using model::StrategyState;

namespace {

auto default_path() -> std::string {
    const char* env = std::getenv("OPTRA_DB");
    if ((env != nullptr) && env[0] != '\0') {
        return env;
    }
    return "data/optra.db";
}

// The schema: the engine writes it (contracts from the IB sync), the backend reads it.
constexpr const char* CREATE_TABLES =
    "CREATE TABLE IF NOT EXISTS contract_equity ("
    "symbol TEXT PRIMARY KEY, name TEXT, product TEXT NOT NULL, multiplier REAL NOT NULL,"
    "price_tick REAL NOT NULL, min_quantity REAL NOT NULL, con_id INTEGER, trading_class TEXT);"
    "CREATE TABLE IF NOT EXISTS contract_option ("
    "symbol TEXT PRIMARY KEY, name TEXT, product TEXT NOT NULL, multiplier REAL NOT NULL,"
    "price_tick REAL NOT NULL, min_quantity REAL NOT NULL, con_id INTEGER, trading_class TEXT,"
    "type TEXT, strike REAL, expiry TEXT, underlying TEXT);"
    "CREATE TABLE IF NOT EXISTS orders ("
    "timestamp TEXT NOT NULL, strategy_name TEXT NOT NULL, order_id TEXT PRIMARY KEY,"
    "symbol TEXT NOT NULL, trading_class TEXT, type TEXT NOT NULL,"
    "side TEXT, price REAL NOT NULL, quantity REAL NOT NULL, filled REAL NOT NULL,"
    "status TEXT NOT NULL, datetime TEXT, reference TEXT, is_combo INTEGER, legs_info TEXT,"
    "combo_type TEXT);"
    "CREATE TABLE IF NOT EXISTS fills ("
    "timestamp TEXT NOT NULL, strategy_name TEXT NOT NULL, fill_id TEXT PRIMARY KEY,"
    "symbol TEXT NOT NULL, order_id TEXT NOT NULL, side TEXT,"
    "price REAL NOT NULL, quantity REAL NOT NULL, datetime TEXT);"
    // setting: name=value;... (each declared parameter); state: what the operator last asked
    "CREATE TABLE IF NOT EXISTS strategies ("
    "name TEXT PRIMARY KEY, class_name TEXT NOT NULL, portfolio TEXT NOT NULL,"
    "setting TEXT NOT NULL, state TEXT NOT NULL, updated TEXT NOT NULL);"
    // The engine's own settings, by name (the risk limits, the halt)
    "CREATE TABLE IF NOT EXISTS settings ("
    "name TEXT PRIMARY KEY, value TEXT NOT NULL, updated TEXT NOT NULL);";

// An order or a fill seen again (a new status, a correction) replaces its row.
constexpr const char* INSERT_ORDER =
    "INSERT OR REPLACE INTO orders (timestamp, strategy_name, order_id, symbol, trading_class,"
    " type, side, price, quantity, filled, status, datetime, reference, is_combo, legs_info,"
    " combo_type) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";
constexpr const char* INSERT_FILL =
    "INSERT OR REPLACE INTO fills (timestamp, strategy_name, fill_id, symbol, order_id,"
    " side, price, quantity, datetime) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)";

constexpr const char* SELECT_ORDERS =
    "SELECT o.strategy_name, o.order_id, o.symbol, o.trading_class, o.type,"
    " o.side, o.price, o.quantity, o.filled, o.status, o.datetime, o.reference, o.is_combo,"
    " o.legs_info, o.combo_type, o.timestamp FROM orders o";
constexpr const char* SELECT_FILLS =
    "SELECT f.strategy_name, f.fill_id, f.symbol, f.order_id, f.side, f.price,"
    " f.quantity, f.datetime, f.timestamp FROM fills f";
constexpr const char* ORDERS_AS_SAVED = "o.timestamp, o.rowid";
constexpr const char* FILLS_AS_SAVED = "f.timestamp, f.rowid";
constexpr const char* ORDERS_LATEST_FIRST = "o.timestamp DESC, o.rowid DESC";
constexpr const char* FILLS_LATEST_FIRST = "f.timestamp DESC, f.rowid DESC";
// A strategy saved again keeps its row, so they load in the order added.
constexpr const char* INSERT_STRATEGY =
    "INSERT INTO strategies (name, class_name, portfolio, setting, state, updated)"
    " VALUES (?, ?, ?, ?, ?, ?) ON CONFLICT (name) DO UPDATE SET class_name = excluded.class_name,"
    " portfolio = excluded.portfolio, setting = excluded.setting, state = excluded.state,"
    " updated = excluded.updated";

// A contract seen again (a later sync) replaces its row.
constexpr const char* INSERT_EQUITY =
    "INSERT OR REPLACE INTO contract_equity (symbol, name, product, multiplier, price_tick,"
    " min_quantity, con_id, trading_class) VALUES (?, ?, ?, ?, ?, ?, ?, ?)";
constexpr const char* INSERT_OPTION =
    "INSERT OR REPLACE INTO contract_option (symbol, name, product, multiplier, price_tick,"
    " min_quantity, con_id, trading_class, type, strike, expiry, underlying)"
    " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";

// The columns both contract tables start with; the option table adds its own after them.
constexpr const char* CONTRACT_COLUMNS =
    "symbol, product, multiplier, price_tick, min_quantity, con_id, trading_class, name";

// ---- SQLite, by hand: a statement that finalizes itself, and the few calls used here
struct Finalize {
    void operator()(sqlite3_stmt* s) const { sqlite3_finalize(s); }
};
using Statement = std::unique_ptr<sqlite3_stmt, Finalize>;

/** Compile one statement; throws with SQLite's message. */
auto prepare(sqlite3* db, std::string_view sql) -> Statement {
    sqlite3_stmt* s = nullptr;
    if (sqlite3_prepare_v2(db, sql.data(), static_cast<int>(sql.size()), &s, nullptr) !=
        SQLITE_OK) {
        throw std::runtime_error(sqlite3_errmsg(db));
    }
    return Statement(s);
}

/** Run SQL that returns nothing needed (DDL, BEGIN / COMMIT); throws with SQLite's message. */
void exec(sqlite3* db, const char* sql) {
    char* err = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        std::string msg = err != nullptr ? err : sqlite3_errmsg(db);
        sqlite3_free(err);
        throw std::runtime_error(msg);
    }
}

// Not named bind: with a std::string argument, ADL would bring in std::bind, a better match.
void bind_value(sqlite3_stmt* s, int i, std::string_view v) {
    sqlite3_bind_text(s, i, v.data(), static_cast<int>(v.size()), SQLITE_TRANSIENT);
}
void bind_value(sqlite3_stmt* s, int i, double v) { sqlite3_bind_double(s, i, v); }
void bind_value(sqlite3_stmt* s, int i, int v) { sqlite3_bind_int(s, i, v); }
template <typename T> void bind_value(sqlite3_stmt* s, int i, const std::optional<T>& v) {
    if (v) {
        bind_value(s, i, *v);
    } else {
        sqlite3_bind_null(s, i);
    }
}

/** Bind the values to ?first, ?first+1, ... in order. */
template <typename... Values> void bind_from(sqlite3_stmt* s, int first, const Values&... values) {
    int i = first - 1;
    (bind_value(s, ++i, values), ...);
}

/** Run a statement whose values are bound, and reset it for the next row. */
void step(sqlite3* db, sqlite3_stmt* s) {
    const int rc = sqlite3_step(s);
    sqlite3_reset(s);
    if (rc != SQLITE_DONE) {
        throw std::runtime_error(sqlite3_errmsg(db));
    }
}

/** Bind to ?1, ?2, ..., run, and reset for the next row. */
template <typename... Values> void run(sqlite3* db, sqlite3_stmt* s, const Values&... values) {
    bind_from(s, 1, values...);
    step(db, s);
}

auto text(sqlite3_stmt* s, int col) -> std::string {
    const unsigned char* t = sqlite3_column_text(s, col);
    return t != nullptr ? reinterpret_cast<const char*>(t) : "";
}
auto is_null(sqlite3_stmt* s, int col) -> bool {
    return sqlite3_column_type(s, col) == SQLITE_NULL;
}

auto option_type_from_string(const std::string& s) -> model::OptionType {
    if (s == "PUT") {
        return model::OptionType::PUT;
    }
    return model::OptionType::CALL;
}

// The expiry column holds the date, YYYY-MM-DD (the New York date the option expires on).
// The option expires at that date's 16:00 New York close, not at midnight in the machine's
// zone.
auto expiry_from_str(const std::string& s) -> std::chrono::system_clock::time_point {
    const std::optional<std::chrono::year_month_day> date = model::parse_date(s);
    return date ? model::option_expiry_time(*date) : std::chrono::system_clock::time_point{};
}

/** The columns of CONTRACT_COLUMNS, from the start of a row. */
auto read_contract(sqlite3_stmt* s) -> model::Contract {
    model::Contract c;
    c.symbol = text(s, 0);
    c.product = model::from_string_product(text(s, 1));
    c.multiplier = sqlite3_column_double(s, 2);
    c.price_tick = sqlite3_column_double(s, 3);
    c.min_quantity = sqlite3_column_double(s, 4);
    c.con_id = sqlite3_column_int(s, 5);
    c.trading_class = text(s, 6);
    c.name = text(s, 7);
    return c;
}

/** The timestamp columns' UTC text back; nullopt for "N/A", empty or anything else. */
auto datetime_from_str(const std::string& s) -> std::optional<model::DateTime> {
    int y = 0;
    unsigned mo = 0;
    unsigned d = 0;
    int h = 0;
    int mi = 0;
    int sec = 0;
    if (std::sscanf(s.c_str(), "%d-%u-%u %d:%d:%d", &y, &mo, &d, &h, &mi, &sec) != 6) {
        return std::nullopt;
    }
    const std::chrono::year_month_day date{std::chrono::year{y}, std::chrono::month{mo},
                                           std::chrono::day{d}};
    if (!date.ok()) {
        return std::nullopt;
    }
    return std::chrono::sys_days{date} + std::chrono::hours{h} + std::chrono::minutes{mi} +
           std::chrono::seconds{sec};
}

/** A side column: nullopt for "N/A". */
auto side_from_str(const std::string& s) -> std::optional<model::Side> {
    if (s.empty() || s == "N/A") {
        return std::nullopt;
    }
    return model::from_string_side(s);
}

auto split(std::string_view s, char by) -> std::vector<std::string_view> {
    std::vector<std::string_view> out;
    while (!s.empty()) {
        const auto at = s.find(by);
        out.push_back(s.substr(0, at));
        if (at == std::string_view::npos) {
            break;
        }
        s.remove_prefix(at + 1);
    }
    return out;
}

/** legs_info's text back into legs. */
auto legs_from_info(std::string_view info) -> std::vector<model::Leg> {
    std::vector<model::Leg> legs;
    for (const std::string_view text : split(info, '|')) {
        model::Leg& leg = legs.emplace_back();
        for (const std::string_view field : split(text, ',')) {
            const auto colon = field.find(':');
            if (colon == std::string_view::npos) {
                continue;
            }
            const std::string_view key = field.substr(0, colon);
            const std::string_view value = field.substr(colon + 1);
            if (key == "con_id") {
                std::from_chars(value.data(), value.data() + value.size(), leg.con_id);
            } else if (key == "ratio") {
                std::from_chars(value.data(), value.data() + value.size(), leg.ratio);
            } else if (key == "dir") {
                leg.side = model::from_string_side(std::string(value));
            } else if (key == "symbol" && value != "N/A") {
                leg.symbol = std::string(value);
            }
        }
    }
    return legs;
}

/** A setting as the strategies table keeps it: name=value;... (each value round-trips). */
auto setting_text(const std::unordered_map<std::string, double>& setting) -> std::string {
    std::vector<std::pair<std::string, double>> sorted(setting.begin(), setting.end());
    std::ranges::sort(sorted);
    std::string out;
    for (const auto& [name, value] : sorted) {
        out += std::format("{}{}={}", out.empty() ? "" : ";", name, value);
    }
    return out;
}

auto setting_from_text(std::string_view text) -> std::unordered_map<std::string, double> {
    std::unordered_map<std::string, double> setting;
    for (const std::string_view field : split(text, ';')) {
        const auto eq = field.find('=');
        if (eq == std::string_view::npos) {
            continue;
        }
        double value = 0.0;
        const std::string_view v = field.substr(eq + 1);
        if (std::from_chars(v.data(), v.data() + v.size(), value).ec == std::errc{}) {
            setting[std::string(field.substr(0, eq))] = value;
        }
    }
    return setting;
}

auto strategy_state_from_string(const std::string& s) -> model::StrategyState {
    if (s == "inited") {
        return model::StrategyState::Inited;
    }
    if (s == "running") {
        return model::StrategyState::Running;
    }
    if (s == "error") {
        return model::StrategyState::Error;
    }
    if (s == "removed") {
        return model::StrategyState::Removed;
    }
    return model::StrategyState::Added;
}

/** A SELECT narrowed to n strategies (`column` IN ?1..?n; none: all) and to `condition` (its ?
 * bound after them; empty: none), ordered by `order`. */
auto narrowed(std::string_view select, std::string_view column, std::size_t n,
              std::string_view condition, std::string_view order) -> std::string {
    std::string where;
    for (std::size_t i = 0; i < n; ++i) {
        where += i == 0 ? std::format("{} IN (?", column) : ", ?";
    }
    where += n > 0 ? ")" : "";
    if (!condition.empty()) {
        where += std::format("{}({})", where.empty() ? "" : " AND ", condition);
    }
    return std::format("{}{}{} ORDER BY {}", select, where.empty() ? "" : " WHERE ", where, order);
}

// An order's legs, as the orders table keeps them.
auto legs_info(const model::Order& order) -> std::string {
    std::string out;
    for (const auto& leg : order.legs) {
        if (!out.empty()) {
            out += "|";
        }
        out += std::format("con_id:{},ratio:{},dir:{},symbol:{}", leg.con_id, leg.ratio,
                           model::to_string(leg.side), leg.symbol.empty() ? "N/A" : leg.symbol);
    }
    return out;
}

} // namespace


SqliteDatabase::SqliteDatabase(utilities::Logger* logger, const std::string& path)
    : log_(logger, "Database"), path_(path.empty() ? default_path() : path) {
    if (sqlite3_open(path_.c_str(), &db_) != SQLITE_OK) {
        log_.write_log(std::format("Cannot open the database {}: {}", path_,
                                   db_ != nullptr ? sqlite3_errmsg(db_) : "out of memory"),
                       utilities::ERROR);
        sqlite3_close(db_);
        db_ = nullptr;
        return;
    }
    // WAL: the backend reads while the engine writes. Wait for a lock instead of failing.
    sqlite3_busy_timeout(db_, 5000);
    try {
        exec(db_, "PRAGMA journal_mode=WAL");
    } catch (const std::exception& e) {
        log_.write_log(std::format("Database {}: no WAL ({})", path_, e.what()),
                       utilities::WARNING);
    }
    create_tables();
    cleanup_expired_options();
    writer_ = std::jthread([this](const std::stop_token& st) { run_writer(st); });
    log_.write_log(std::format("Database {} opened (SQLite)", path_), utilities::INFO);
}

SqliteDatabase::~SqliteDatabase() { SqliteDatabase::close(); }

void SqliteDatabase::create_tables() {
    std::scoped_lock lock(db_mutex_);
    try {
        exec(db_, CREATE_TABLES);
    } catch (const std::exception& e) {
        log_.write_log(std::string("Create tables failed: ") + e.what(), utilities::ERROR);
    }
}

void SqliteDatabase::cleanup_expired_options() {
    std::scoped_lock lock(db_mutex_);
    try {
        const std::string today = std::format(
            "{:%Y-%m-%d}", std::chrono::floor<std::chrono::days>(std::chrono::system_clock::now()));
        Statement del = prepare(db_, "DELETE FROM contract_option WHERE expiry < ?");
        run(db_, del.get(), today);
        if (const int count = sqlite3_changes(db_); count > 0) {
            log_.write_log(std::format("Cleaned up {} expired option contracts", count),
                           utilities::INFO);
        }
    } catch (const std::exception& e) {
        log_.write_log(std::string("Cleanup expired options failed: ") + e.what(),
                       utilities::ERROR);
    }
}

auto SqliteDatabase::load_option_contracts() -> std::vector<model::Contract> {
    std::scoped_lock lock(db_mutex_);
    std::vector<model::Contract> out;
    if (db_ == nullptr) {
        return out;
    }
    try {
        Statement s = prepare(db_, std::format("SELECT {}, type, strike, expiry, underlying"
                                               " FROM contract_option",
                                               CONTRACT_COLUMNS));
        while (sqlite3_step(s.get()) == SQLITE_ROW) {
            model::Contract c = read_contract(s.get());
            if (!is_null(s.get(), 8)) {
                c.option_type = option_type_from_string(text(s.get(), 8));
            }
            if (!is_null(s.get(), 9)) {
                c.option_strike = sqlite3_column_double(s.get(), 9);
            }
            if (!is_null(s.get(), 10)) {
                c.option_expiry = expiry_from_str(text(s.get(), 10));
            }
            if (!is_null(s.get(), 11)) {
                c.option_underlying = text(s.get(), 11);
            }
            out.push_back(std::move(c));
        }
    } catch (const std::exception& e) {
        log_.write_log(std::string("Failed to load option Contract: ") + e.what(),
                       utilities::ERROR);
    }
    return out;
}

auto SqliteDatabase::load_equity_contracts() -> std::vector<model::Contract> {
    std::scoped_lock lock(db_mutex_);
    std::vector<model::Contract> out;
    if (db_ == nullptr) {
        return out;
    }
    try {
        Statement s = prepare(db_, std::format("SELECT {} FROM contract_equity", CONTRACT_COLUMNS));
        while (sqlite3_step(s.get()) == SQLITE_ROW) {
            out.push_back(read_contract(s.get()));
        }
    } catch (const std::exception& e) {
        log_.write_log(std::string("Failed to load equity Contract: ") + e.what(),
                       utilities::ERROR);
    }
    return out;
}

void SqliteDatabase::save_contracts(std::span<const model::Contract> contracts) {
    std::scoped_lock lock(db_mutex_);
    if (db_ == nullptr) {
        throw std::runtime_error(std::format("No database at {}", path_));
    }
    try {
        Statement equity = prepare(db_, INSERT_EQUITY);
        Statement option = prepare(db_, INSERT_OPTION);
        exec(db_, "BEGIN");
        for (const model::Contract& c : contracts) {
            const bool is_option = c.product == model::Product::OPTION;
            sqlite3_stmt* s = is_option ? option.get() : equity.get();
            bind_from(s, 1, c.symbol, c.name, model::to_string(c.product), c.multiplier,
                      c.price_tick, c.min_quantity, c.con_id, c.trading_class);
            if (is_option) {
                const auto type = c.option_type.transform(
                    [](model::OptionType t) { return model::to_string(t); });
                const auto expiry = c.option_expiry.transform([](model::DateTime t) {
                    return std::format("{:%F}", model::new_york_date(t));
                });
                bind_from(s, 9, type, c.option_strike, expiry, c.option_underlying);
            }
            step(db_, s);
        }
        exec(db_, "COMMIT");
    } catch (const std::exception& e) {
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        throw std::runtime_error(
            std::format("Failed to save {} contracts: {}", contracts.size(), e.what()));
    }
}

void SqliteDatabase::save_order(const std::string& strategy_name,
                                const model::Order& order) {
    queue({strategy_name, std::chrono::system_clock::now(), order});
}

void SqliteDatabase::save_fill(const std::string& strategy_name,
                               const model::Fill& fill) {
    queue({strategy_name, std::chrono::system_clock::now(), fill});
}

void SqliteDatabase::queue(SavedRecord pending) {
    if (db_ == nullptr) {
        return;
    }
    bool many = false;
    {
        std::scoped_lock lock(pending_mutex_);
        pending_.push_back(std::move(pending));
        many = pending_.size() == kWriteBatch;
    }
    if (many) { // else the writer comes round within kWriteEvery: no wake-up for each record
        pending_cv_.notify_one();
    }
}

void SqliteDatabase::run_writer(const std::stop_token& st) {
    for (;;) {
        {
            std::unique_lock lock(pending_mutex_);
            pending_cv_.wait_for(lock, st, kWriteEvery,
                                 [this] { return pending_.size() >= kWriteBatch; });
            if (pending_.empty()) {
                if (st.stop_requested()) {
                    return; // stopped with nothing left to write
                }
                continue;
            }
        }
        std::scoped_lock lock(db_mutex_);
        write_pending();
    }
}

// One transaction per batch: a burst of fills costs one disk sync, not one each.
void SqliteDatabase::write_pending() {
    std::vector<SavedRecord>& batch = writing_; // empty here; both keep their capacity
    {
        std::scoped_lock lock(pending_mutex_);
        batch.swap(pending_);
    }
    if (batch.empty()) {
        return;
    }
    try {
        Statement order_stmt = prepare(db_, INSERT_ORDER);
        Statement trade_stmt = prepare(db_, INSERT_FILL);
        exec(db_, "BEGIN");
        for (const SavedRecord& p : batch) {
            const std::string ts = datetime_to_str(p.saved_at);
            if (const auto* o = std::get_if<model::Order>(&p.record)) {
                // side: a contract's own order's side, a combo's BUY (a BAG is bought).
                run(db_, order_stmt.get(), ts, p.strategy_name, o->order_id, o->symbol,
                    o->trading_class, model::to_string(o->type),
                    model::to_string(o->is_combo() || o->legs.empty()
                                         ? model::Side::BUY
                                         : o->legs.front().side),
                    o->price, o->quantity, o->filled, model::to_string(o->status),
                    o->datetime != model::DateTime{} ? datetime_to_str(o->datetime) : "N/A",
                    o->reference, o->is_combo() ? 1 : 0, legs_info(*o),
                    model::to_string(o->combo_type));
            } else {
                const auto& t = std::get<model::Fill>(p.record);
                run(db_, trade_stmt.get(), ts, p.strategy_name, t.fill_id, t.symbol, t.order_id,
                    model::to_string(t.side), t.price, t.quantity,
                    t.datetime != model::DateTime{} ? datetime_to_str(t.datetime) : "");
            }
        }
        exec(db_, "COMMIT");
    } catch (const std::exception& e) {
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        log_.write_log(std::format("Failed to save {} orders / fills: {}", batch.size(), e.what()),
                       utilities::ERROR);
    }
    batch.clear();
}

template <class Decode>
auto SqliteDatabase::read_records(const std::string& sql, const std::vector<std::string>& values,
                                  const char* what, Decode decode)
    -> std::vector<std::invoke_result_t<Decode, sqlite3_stmt*>> {
    std::scoped_lock lock(db_mutex_);
    std::vector<std::invoke_result_t<Decode, sqlite3_stmt*>> out;
    if (db_ == nullptr) {
        return out;
    }
    write_pending(); // every record saved before the read
    try {
        Statement s = prepare(db_, sql);
        for (std::size_t i = 0; i < values.size(); ++i) {
            bind_value(s.get(), static_cast<int>(i) + 1, values[i]);
        }
        while (sqlite3_step(s.get()) == SQLITE_ROW) {
            out.push_back(decode(s.get()));
        }
    } catch (const std::exception& e) {
        log_.write_log(std::format("Failed to load the {}: {}", what, e.what()), utilities::ERROR);
    }
    return out;
}

auto SqliteDatabase::load_orders(const std::vector<std::string>& strategies, std::size_t latest)
    -> std::vector<core::SavedOrder> {
    std::string sql = narrowed(SELECT_ORDERS, "o.strategy_name", strategies.size(), "",
                               latest == 0 ? ORDERS_AS_SAVED : ORDERS_LATEST_FIRST);
    if (latest > 0) {
        sql += std::format(" LIMIT {}", latest);
    }
    std::vector<core::SavedOrder> out = read_records(sql, strategies, "orders", [](sqlite3_stmt* s) {
        model::Order o;
        o.order_id = text(s, 1);
        o.symbol = text(s, 2);
        o.trading_class = text(s, 3);
        o.type = model::from_string_ordertype(text(s, 4));
        o.price = sqlite3_column_double(s, 6);
        o.quantity = sqlite3_column_double(s, 7);
        o.filled = sqlite3_column_double(s, 8);
        o.status = model::from_string_status(text(s, 9));
        o.datetime = datetime_from_str(text(s, 10)).value_or(model::DateTime{});
        o.reference = text(s, 11);
        o.legs = legs_from_info(text(s, 13));
        for (model::Leg& leg : o.legs) {
            leg.trading_class = o.trading_class;
        }
        if (!is_null(s, 14)) {
            o.combo_type = model::from_string_combo(text(s, 14));
        }
        return core::SavedOrder{
            .strategy_name = text(s, 0),
            .saved_at = datetime_from_str(text(s, 15)).value_or(model::DateTime{}),
            .record = std::move(o)};
    });
    if (latest > 0) {
        std::ranges::reverse(out);
    }
    return out;
}

auto SqliteDatabase::load_fills(const std::vector<std::string>& strategies, std::size_t latest)
    -> std::vector<core::SavedFill> {
    std::string sql = narrowed(SELECT_FILLS, "f.strategy_name", strategies.size(), "",
                               latest == 0 ? FILLS_AS_SAVED : FILLS_LATEST_FIRST);
    if (latest > 0) {
        sql += std::format(" LIMIT {}", latest);
    }
    std::vector<core::SavedFill> out = read_records(sql, strategies, "fills", [](sqlite3_stmt* s) {
        model::Fill f;
        f.fill_id = text(s, 1);
        f.symbol = text(s, 2);
        f.order_id = text(s, 3);
        f.side = side_from_str(text(s, 4)).value_or(model::Side::BUY);
        f.price = sqlite3_column_double(s, 5);
        f.quantity = sqlite3_column_double(s, 6);
        f.datetime = datetime_from_str(text(s, 7)).value_or(model::DateTime{});
        return core::SavedFill{
            .strategy_name = text(s, 0),
            .saved_at = datetime_from_str(text(s, 8)).value_or(model::DateTime{}),
            .record = std::move(f)};
    });
    if (latest > 0) {
        std::ranges::reverse(out);
    }
    return out;
}

auto SqliteDatabase::strategies_with_records() -> std::vector<std::string> {
    std::scoped_lock lock(db_mutex_);
    std::vector<std::string> out;
    if (db_ == nullptr) {
        return out;
    }
    write_pending(); // every record saved before the read
    try {
        Statement s = prepare(db_, "SELECT strategy_name FROM orders"
                                   " UNION SELECT strategy_name FROM fills ORDER BY 1");
        while (sqlite3_step(s.get()) == SQLITE_ROW) {
            out.push_back(text(s.get(), 0));
        }
    } catch (const std::exception& e) {
        log_.write_log(std::format("Failed to list the strategies with records: {}", e.what()),
                       utilities::ERROR);
    }
    return out;
}

auto SqliteDatabase::delete_orders_and_fills() -> std::vector<std::pair<std::string, int>> {
    static constexpr std::array<const char*, 2> kTables{"orders", "fills"};
    std::scoped_lock lock(db_mutex_);
    write_pending();
    return delete_rows(kTables);
}

auto SqliteDatabase::delete_contracts() -> std::vector<std::pair<std::string, int>> {
    static constexpr std::array<const char*, 2> kTables{"contract_option", "contract_equity"};
    std::scoped_lock lock(db_mutex_);
    return delete_rows(kTables);
}

auto SqliteDatabase::delete_rows(std::span<const char* const> tables)
    -> std::vector<std::pair<std::string, int>> {
    if (db_ == nullptr) {
        throw std::runtime_error(std::format("No database at {}", path_));
    }
    std::vector<std::pair<std::string, int>> out;
    try {
        exec(db_, "BEGIN");
        for (const char* table : tables) {
            exec(db_, std::format("DELETE FROM {}", table).c_str());
            out.emplace_back(table, sqlite3_changes(db_));
        }
        exec(db_, "COMMIT");
    } catch (const std::exception&) {
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        throw;
    }
    std::string emptied;
    for (const auto& [table, rows] : out) {
        emptied += std::format("{}{} {}", emptied.empty() ? "" : ", ", rows, table);
    }
    log_.write_log(std::format("Database: emptied ({})", emptied), utilities::INFO);
    return out;
}

void SqliteDatabase::save_setting(const std::string& name, const std::string& value) {
    std::scoped_lock lock(db_mutex_);
    if (db_ == nullptr) {
        return;
    }
    try {
        Statement s = prepare(
            db_, "INSERT OR REPLACE INTO settings (name, value, updated) VALUES (?, ?, ?)");
        run(db_, s.get(), name, value, datetime_to_str(std::chrono::system_clock::now()));
    } catch (const std::exception& e) {
        log_.write_log(std::format("Failed to save the setting {}: {}", name, e.what()),
                       utilities::ERROR);
    }
}

auto SqliteDatabase::load_settings() -> std::unordered_map<std::string, std::string> {
    std::scoped_lock lock(db_mutex_);
    std::unordered_map<std::string, std::string> out;
    if (db_ == nullptr) {
        return out;
    }
    try {
        Statement s = prepare(db_, "SELECT name, value FROM settings");
        while (sqlite3_step(s.get()) == SQLITE_ROW) {
            out[text(s.get(), 0)] = text(s.get(), 1);
        }
    } catch (const std::exception& e) {
        log_.write_log(std::format("Failed to load the settings: {}", e.what()), utilities::ERROR);
    }
    return out;
}

void SqliteDatabase::save_strategy(const model::StrategyRecord& record) {
    std::scoped_lock lock(db_mutex_);
    if (db_ == nullptr) {
        return;
    }
    try {
        Statement s = prepare(db_, INSERT_STRATEGY);
        run(db_, s.get(), record.name, record.class_name, record.portfolio,
            setting_text(record.setting), to_string(record.state),
            datetime_to_str(std::chrono::system_clock::now()));
    } catch (const std::exception& e) {
        log_.write_log(std::format("Failed to save {}: {}", record.name, e.what()),
                       utilities::ERROR);
    }
}

void SqliteDatabase::delete_strategy(const std::string& name) {
    std::scoped_lock lock(db_mutex_);
    if (db_ == nullptr) {
        return;
    }
    try {
        Statement s = prepare(db_, "DELETE FROM strategies WHERE name = ?");
        run(db_, s.get(), name);
    } catch (const std::exception& e) {
        log_.write_log(std::format("Failed to delete {}: {}", name, e.what()), utilities::ERROR);
    }
}

auto SqliteDatabase::load_strategies() -> std::vector<model::StrategyRecord> {
    std::scoped_lock lock(db_mutex_);
    std::vector<model::StrategyRecord> out;
    if (db_ == nullptr) {
        return out;
    }
    try {
        Statement s = prepare(db_, "SELECT name, class_name, portfolio, setting, state"
                                   " FROM strategies ORDER BY rowid");
        while (sqlite3_step(s.get()) == SQLITE_ROW) {
            out.push_back(model::StrategyRecord{
                .name = text(s.get(), 0),
                .class_name = text(s.get(), 1),
                .portfolio = text(s.get(), 2),
                .setting = setting_from_text(text(s.get(), 3)),
                .state = strategy_state_from_string(text(s.get(), 4)),
            });
        }
    } catch (const std::exception& e) {
        log_.write_log(std::format("Failed to load the strategies: {}", e.what()),
                       utilities::ERROR);
    }
    return out;
}

auto SqliteDatabase::is_strategy_name_used(const std::string& name) -> bool {
    {
        std::scoped_lock lock(pending_mutex_); // not written yet
        if (std::ranges::any_of(pending_,
                                [&](const SavedRecord& p) { return p.strategy_name == name; })) {
            return true;
        }
    }
    std::scoped_lock lock(db_mutex_);
    if (db_ == nullptr) {
        throw std::runtime_error("The database is closed");
    }
    Statement s = prepare(db_, "SELECT EXISTS (SELECT 1 FROM strategies WHERE name = ?1)"
                               " OR EXISTS (SELECT 1 FROM orders WHERE strategy_name = ?1)"
                               " OR EXISTS (SELECT 1 FROM fills WHERE strategy_name = ?1)");
    bind_from(s.get(), 1, name);
    if (sqlite3_step(s.get()) != SQLITE_ROW) {
        throw std::runtime_error(sqlite3_errmsg(db_));
    }
    return sqlite3_column_int(s.get(), 0) != 0;
}

void SqliteDatabase::close() {
    if (writer_.joinable()) { // it writes what is queued before it stops
        writer_.request_stop();
        writer_.join();
    }
    std::scoped_lock lock(db_mutex_);
    if (db_ != nullptr) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

} // namespace adapters
