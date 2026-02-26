/**
 * DatabaseEngine (live): PostgreSQL 持久化（libpqxx），模仿 engines/engine_db.py。
 * load_contracts 发出 Contract 事件（先 equity 后 option），由 EventEngine → MarketDataEngine 建立
 * portfolio。
 */

#include "engine_db_pg.hpp"
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <pqxx/pqxx>
#include <sstream>
#include <stdexcept>

namespace engines {

namespace {

auto default_conninfo() -> std::string {
    const char* env = std::getenv("DATABASE_URL");
    if ((env != nullptr) && env[0] != '\0') {
        return env;
    }
    return "dbname=trading";
}

// PostgreSQL DDL（与 Python sql_schema 对应，类型改为 PG 兼容）
const char* CREATE_CONTRACT_EQUITY =
    "CREATE TABLE IF NOT EXISTS contract_equity ("
    "symbol TEXT PRIMARY KEY, exchange TEXT NOT NULL, name TEXT, product TEXT NOT NULL,"
    "size DOUBLE PRECISION NOT NULL, pricetick DOUBLE PRECISION NOT NULL, min_volume DOUBLE "
    "PRECISION NOT NULL,"
    "net_position INTEGER NOT NULL, history_data INTEGER NOT NULL, stop_supported INTEGER NOT NULL,"
    "gateway_name TEXT NOT NULL, con_id INTEGER, trading_class TEXT, max_volume DOUBLE PRECISION, "
    "extra TEXT)";

const char* CREATE_CONTRACT_OPTION =
    "CREATE TABLE IF NOT EXISTS contract_option ("
    "symbol TEXT PRIMARY KEY, exchange TEXT NOT NULL, name TEXT, product TEXT NOT NULL,"
    "size DOUBLE PRECISION NOT NULL, pricetick DOUBLE PRECISION NOT NULL, min_volume DOUBLE "
    "PRECISION NOT NULL,"
    "net_position INTEGER NOT NULL, history_data INTEGER NOT NULL, stop_supported INTEGER NOT NULL,"
    "gateway_name TEXT NOT NULL, con_id INTEGER, trading_class TEXT, max_volume DOUBLE PRECISION, "
    "extra TEXT,"
    "portfolio TEXT, type TEXT, strike DOUBLE PRECISION, strike_index TEXT, expiry TEXT, "
    "underlying TEXT)";

const char* CREATE_ORDERS =
    "CREATE TABLE IF NOT EXISTS orders ("
    "timestamp TEXT NOT NULL, strategy_name TEXT NOT NULL, orderid TEXT PRIMARY KEY, symbol TEXT "
    "NOT NULL,"
    "exchange TEXT NOT NULL, trading_class TEXT, type TEXT NOT NULL, direction TEXT,"
    "price DOUBLE PRECISION NOT NULL, volume DOUBLE PRECISION NOT NULL, traded DOUBLE PRECISION "
    "NOT NULL,"
    "status TEXT NOT NULL, datetime TEXT, reference TEXT, is_combo INTEGER, legs_info TEXT)";

const char* CREATE_TRADES =
    "CREATE TABLE IF NOT EXISTS trades ("
    "timestamp TEXT NOT NULL, strategy_name TEXT NOT NULL, tradeid TEXT PRIMARY KEY, symbol TEXT "
    "NOT NULL,"
    "exchange TEXT NOT NULL, orderid TEXT NOT NULL, direction TEXT,"
    "price DOUBLE PRECISION NOT NULL, volume DOUBLE PRECISION NOT NULL, datetime TEXT)";

auto exchange_from_string(const std::string& s) -> utilities::Exchange {
    if (s == "SMART") {
        return utilities::Exchange::SMART;
    }
    if (s == "NYSE") {
        return utilities::Exchange::NYSE;
    }
    if (s == "NASDAQ") {
        return utilities::Exchange::NASDAQ;
    }
    if (s == "AMEX") {
        return utilities::Exchange::AMEX;
    }
    if (s == "CBOE") {
        return utilities::Exchange::CBOE;
    }
    if (s == "IBKRATS") {
        return utilities::Exchange::IBKRATS;
    }
    return utilities::Exchange::LOCAL;
}

auto product_from_string(const std::string& s) -> utilities::Product {
    if (s == "EQUITY") {
        return utilities::Product::EQUITY;
    }
    if (s == "FUTURES") {
        return utilities::Product::FUTURES;
    }
    if (s == "OPTION") {
        return utilities::Product::OPTION;
    }
    if (s == "INDEX") {
        return utilities::Product::INDEX;
    }
    if (s == "FOREX") {
        return utilities::Product::FOREX;
    }
    if (s == "SPOT") {
        return utilities::Product::SPOT;
    }
    if (s == "ETF") {
        return utilities::Product::ETF;
    }
    if (s == "BOND") {
        return utilities::Product::BOND;
    }
    if (s == "WARRANT") {
        return utilities::Product::WARRANT;
    }
    if (s == "SPREAD") {
        return utilities::Product::SPREAD;
    }
    if (s == "FUND") {
        return utilities::Product::FUND;
    }
    if (s == "CFD") {
        return utilities::Product::CFD;
    }
    if (s == "SWAP") {
        return utilities::Product::SWAP;
    }
    return utilities::Product::UNKNOWN;
}

auto option_type_from_string(const std::string& s) -> utilities::OptionType {
    if (s == "PUT") {
        return utilities::OptionType::PUT;
    }
    return utilities::OptionType::CALL;
}

auto datetime_to_str(const std::chrono::system_clock::time_point& tp) -> std::string {
    auto t = std::chrono::system_clock::to_time_t(tp);
    std::tm* tm = std::gmtime(&t);
    if (tm == nullptr) {
        return "";
    }
    std::ostringstream os;
    os << std::put_time(tm, "%Y-%m-%d %H:%M:%S");
    return os.str();
}

auto date_to_str(const std::chrono::system_clock::time_point& tp) -> std::string {
    auto t = std::chrono::system_clock::to_time_t(tp);
    std::tm* tm = std::gmtime(&t);
    if (tm == nullptr) {
        return "";
    }
    std::ostringstream os;
    os << std::put_time(tm, "%Y-%m-%d");
    return os.str();
}

auto str_to_datetime(const std::string& s) -> std::chrono::system_clock::time_point {
    if (s.empty()) {
        return {};
    }
    std::tm tm = {};
    int y;
    int M;
    int d;
    int h;
    int m;
    int sec;
    if (std::sscanf(s.c_str(), "%d-%d-%d %d:%d:%d", &y, &M, &d, &h, &m, &sec) >= 6) {
        tm.tm_year = y - 1900;
        tm.tm_mon = M - 1;
        tm.tm_mday = d;
        tm.tm_hour = h;
        tm.tm_min = m;
        tm.tm_sec = sec;
        return std::chrono::system_clock::from_time_t(std::mktime(&tm));
    }
    if (std::sscanf(s.c_str(), "%d-%d-%d", &y, &M, &d) >= 3) {
        tm.tm_year = y - 1900;
        tm.tm_mon = M - 1;
        tm.tm_mday = d;
        return std::chrono::system_clock::from_time_t(std::mktime(&tm));
    }
    return {};
}

} // namespace

DatabaseEngine::DatabaseEngine(utilities::MainEngine* main_engine, const std::string& conninfo)
    : BaseEngine(main_engine, "Database"),
      conninfo_(conninfo.empty() ? default_conninfo() : conninfo) {
    try {
        conn_ = std::make_unique<pqxx::connection>(conninfo_);
        bool created = false;
        {
            pqxx::work w(*conn_);
            auto r = w.exec("SELECT 1 FROM pg_tables WHERE schemaname='public' AND "
                            "tablename='contract_equity'");
            if (r.empty()) {
                created = true;
            }
            w.commit();
        }
        if (created) {
            write_log("Database file not found, creating new database", INFO);
            create_tables();
        } else {
            cleanup_expired_options();
        }
        write_log("Database engine initialized (PostgreSQL)", INFO);
    } catch (const std::exception& e) {
        write_log(std::string("Database init failed: ") + e.what(), ERROR);
        conn_.reset();
    }
}

DatabaseEngine::~DatabaseEngine() { close(); }

void DatabaseEngine::write_log(const std::string& msg, int level) {
    if (main_engine != nullptr) {
        main_engine->write_log(msg, level, engine_name);
    }
}

void DatabaseEngine::create_tables() {
    std::scoped_lock lock(db_mutex_);
    if (!conn_) {
        return;
    }
    try {
        pqxx::work w(*conn_);
        w.exec(CREATE_CONTRACT_EQUITY);
        w.exec(CREATE_CONTRACT_OPTION);
        w.exec(CREATE_ORDERS);
        w.exec(CREATE_TRADES);
        w.commit();
        write_log("All tables created successfully", INFO);
    } catch (const std::exception& e) {
        write_log(std::string("Create tables failed: ") + e.what(), ERROR);
    }
}

void DatabaseEngine::cleanup_expired_options() {
    std::scoped_lock lock(db_mutex_);
    if (!conn_) {
        return;
    }
    try {
        auto now = std::chrono::system_clock::now();
        std::string today = date_to_str(now);
        pqxx::work w(*conn_);
        auto r = w.exec(pqxx::zview("SELECT COUNT(*) FROM contract_option WHERE expiry < $1"),
                        pqxx::params{today});
        long count = r.empty() ? 0 : r[0][0].as<long>();
        if (count > 0) {
            w.exec(pqxx::zview("DELETE FROM contract_option WHERE expiry < $1"),
                   pqxx::params{today});
            w.commit();
            write_log("Cleaned up " + std::to_string(count) + " expired option contracts", INFO);
        } else {
            w.commit();
        }
    } catch (const std::exception& e) {
        write_log(std::string("Cleanup expired options failed: ") + e.what(), ERROR);
    }
}

void DatabaseEngine::load_contracts() {
    if (main_engine == nullptr) {
        return;
    }
    auto contracts = load_contract_data(nullptr);
    for (auto& kv : contracts) {
        main_engine->put_event(utilities::Event(utilities::EventType::Contract, kv.second));
    }
    write_log("Loaded " + std::to_string(contracts.size()) + " contracts", INFO);
}

void DatabaseEngine::save_contract_data(const utilities::ContractData& contract,
                                        const std::string& symbol_key) {
    std::scoped_lock lock(db_mutex_);
    if (!conn_) {
        return;
    }
    try {
        std::string ex = utilities::to_string(contract.exchange);
        std::string prod = utilities::to_string(contract.product);
        int con_id = contract.con_id.value_or(0);
        double max_vol = contract.max_volume.value_or(0.0);

        if (contract.product == utilities::Product::OPTION) {
            std::string opt_type =
                contract.option_type ? utilities::to_string(*contract.option_type) : "";
            double strike = contract.option_strike.value_or(0.0);
            std::string expiry = contract.option_expiry ? date_to_str(*contract.option_expiry) : "";
            std::string underlying = contract.option_underlying.value_or("");
            std::string portfolio = contract.option_portfolio.value_or("");
            std::string strike_index = contract.option_index.value_or("");

            pqxx::work w(*conn_);
            w.exec(pqxx::zview("INSERT INTO contract_option (symbol, exchange, name, product, "
                               "size, pricetick, min_volume, "
                               "net_position, history_data, stop_supported, gateway_name, con_id, "
                               "trading_class, max_volume, extra, "
                               "portfolio, type, strike, strike_index, expiry, underlying) "
                               "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12, $13, "
                               "$14, NULL, $15, $16, $17, $18, $19, $20) "
                               "ON CONFLICT (symbol) DO UPDATE SET exchange=$2, name=$3, "
                               "product=$4, size=$5, pricetick=$6, min_volume=$7, "
                               "net_position=$8, history_data=$9, stop_supported=$10, "
                               "gateway_name=$11, con_id=$12, trading_class=$13, "
                               "max_volume=$14, portfolio=$15, type=$16, strike=$17, "
                               "strike_index=$18, expiry=$19, underlying=$20"),
                   pqxx::params{symbol_key,
                                ex,
                                contract.name,
                                prod,
                                contract.size,
                                contract.pricetick,
                                contract.min_volume,
                                contract.net_position ? 1 : 0,
                                contract.history_data ? 1 : 0,
                                contract.stop_supported ? 1 : 0,
                                contract.gateway_name,
                                con_id,
                                contract.trading_class.value_or(""),
                                max_vol,
                                portfolio,
                                opt_type,
                                strike,
                                strike_index,
                                expiry,
                                underlying});
            w.commit();
        } else {
            pqxx::work w(*conn_);
            w.exec(pqxx::zview(
                       "INSERT INTO contract_equity (symbol, exchange, name, product, size, "
                       "pricetick, min_volume, "
                       "net_position, history_data, stop_supported, gateway_name, con_id, "
                       "trading_class, max_volume, extra) "
                       "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12, $13, $14, NULL) "
                       "ON CONFLICT (symbol) DO UPDATE SET exchange=$2, name=$3, product=$4, "
                       "size=$5, pricetick=$6, min_volume=$7, "
                       "net_position=$8, history_data=$9, stop_supported=$10, gateway_name=$11, "
                       "con_id=$12, trading_class=$13, max_volume=$14"),
                   pqxx::params{symbol_key, ex, contract.name, prod, contract.size,
                                contract.pricetick, contract.min_volume,
                                contract.net_position ? 1 : 0, contract.history_data ? 1 : 0,
                                contract.stop_supported ? 1 : 0, contract.gateway_name, con_id,
                                contract.trading_class.value_or(""), max_vol});
            w.commit();
        }
    } catch (const std::exception& e) {
        write_log(std::string("Failed to save ContractData: ") + e.what(), ERROR);
    }
}

auto DatabaseEngine::load_contract_data(const std::string* symbol_key)
    -> std::unordered_map<std::string, utilities::ContractData> {
    std::scoped_lock lock(db_mutex_);
    std::unordered_map<std::string, utilities::ContractData> contracts;
    if (!conn_) {
        return contracts;
    }
    try {
        pqxx::work w(*conn_);
        std::string eq_sql = "SELECT symbol, exchange, product, size, pricetick, min_volume, "
                             "net_position, history_data, "
                             "stop_supported, gateway_name, con_id, trading_class, name, "
                             "max_volume FROM contract_equity";
        pqxx::result r;
        if (symbol_key != nullptr) {
            r = w.exec(pqxx::zview(eq_sql + " WHERE symbol = $1"), pqxx::params{*symbol_key});
        } else {
            r = w.exec(eq_sql);
        }

        for (auto row : r) {
            utilities::ContractData c;
            std::string row_sym = row[0].as<std::string>();
            c.symbol = row[0].as<std::string>();
            c.exchange = exchange_from_string(row[1].as<std::string>());
            c.product = product_from_string(row[2].as<std::string>());
            c.size = row[3].as<double>();
            c.pricetick = row[4].as<double>();
            c.min_volume = row[5].as<double>();
            c.net_position = row[6].as<int>() != 0;
            c.history_data = row[7].as<int>() != 0;
            c.stop_supported = row[8].as<int>() != 0;
            c.gateway_name = row[9].as<std::string>();
            c.con_id = row[10].as<int>(0);
            if (!row[11].is_null()) {
                c.trading_class = row[11].as<std::string>();
            }
            c.name = row[12].as<std::string>();
            if (!row[13].is_null()) {
                c.max_volume = row[13].as<double>();
            }
            contracts[row_sym] = std::move(c);
        }

        std::string opt_sql =
            "SELECT symbol, exchange, product, size, pricetick, min_volume, net_position, "
            "history_data, "
            "stop_supported, gateway_name, con_id, trading_class, name, max_volume, portfolio, "
            "type, strike, strike_index, expiry, underlying FROM contract_option";
        if (symbol_key != nullptr) {
            r = w.exec(pqxx::zview(opt_sql + " WHERE symbol = $1"), pqxx::params{*symbol_key});
        } else {
            r = w.exec(opt_sql);
        }

        for (auto row : r) {
            utilities::ContractData c;
            std::string row_sym = row[0].as<std::string>();
            c.symbol = row[0].as<std::string>();
            c.exchange = exchange_from_string(row[1].as<std::string>());
            c.product = product_from_string(row[2].as<std::string>());
            c.size = row[3].as<double>();
            c.pricetick = row[4].as<double>();
            c.min_volume = row[5].as<double>();
            c.net_position = row[6].as<int>() != 0;
            c.history_data = row[7].as<int>() != 0;
            c.stop_supported = row[8].as<int>() != 0;
            c.gateway_name = row[9].as<std::string>();
            c.con_id = row[10].as<int>(0);
            if (!row[11].is_null()) {
                c.trading_class = row[11].as<std::string>();
            }
            c.name = row[12].as<std::string>();
            if (!row[13].is_null()) {
                c.max_volume = row[13].as<double>();
            }
            if (row.size() > 14 && !row[14].is_null()) {
                c.option_portfolio = row[14].as<std::string>();
            }
            if (row.size() > 15 && !row[15].is_null()) {
                c.option_type = option_type_from_string(row[15].as<std::string>());
            }
            if (row.size() > 16 && !row[16].is_null()) {
                c.option_strike = row[16].as<double>();
            }
            if (row.size() > 17 && !row[17].is_null()) {
                c.option_index = row[17].as<std::string>();
            }
            if (row.size() > 18 && !row[18].is_null()) {
                c.option_expiry = str_to_datetime(row[18].as<std::string>());
            }
            if (row.size() > 19 && !row[19].is_null()) {
                c.option_underlying = row[19].as<std::string>();
            }
            contracts[row_sym] = std::move(c);
        }
        w.commit();
    } catch (const std::exception& e) {
        write_log(std::string("Failed to load ContractData: ") + e.what(), ERROR);
    }
    return contracts;
}

void DatabaseEngine::save_order_data(const std::string& strategy_name,
                                     const utilities::OrderData& order) {
    std::scoped_lock lock(db_mutex_);
    if (!conn_) {
        return;
    }
    try {
        std::string legs_info;
        if (order.is_combo && order.legs) {
            for (size_t i = 0; i < order.legs->size(); ++i) {
                if (i != 0U) {
                    legs_info += "|";
                }
                const auto& leg = (*order.legs)[i];
                legs_info += "con_id:" + std::to_string(leg.con_id) +
                             ",ratio:" + std::to_string(leg.ratio) +
                             ",dir:" + utilities::to_string(leg.direction) +
                             ",symbol:" + (leg.symbol.value_or("N/A"));
            }
        }
        std::string ts = datetime_to_str(std::chrono::system_clock::now());
        std::string dir = order.direction ? utilities::to_string(*order.direction) : "N/A";
        std::string dt = order.datetime ? datetime_to_str(*order.datetime) : "N/A";

        pqxx::work w(*conn_);
        w.exec(pqxx::zview(
                   "INSERT INTO orders (timestamp, strategy_name, orderid, symbol, exchange, "
                   "trading_class, type, direction, "
                   "price, volume, traded, status, datetime, reference, is_combo, legs_info) "
                   "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12, $13, $14, $15, $16) "
                   "ON CONFLICT (orderid) DO UPDATE SET timestamp=$1, strategy_name=$2, symbol=$4, "
                   "exchange=$5, trading_class=$6, "
                   "type=$7, direction=$8, price=$9, volume=$10, traded=$11, status=$12, "
                   "datetime=$13, reference=$14, is_combo=$15, legs_info=$16"),
               pqxx::params{ts, strategy_name, order.orderid, order.symbol,
                            utilities::to_string(order.exchange), order.trading_class.value_or(""),
                            utilities::to_string(order.type), dir, order.price, order.volume,
                            order.traded, utilities::to_string(order.status), dt, order.reference,
                            order.is_combo ? 1 : 0, legs_info});
        w.commit();
    } catch (const std::exception& e) {
        write_log(std::string("Failed to save order: ") + e.what(), ERROR);
    }
}

void DatabaseEngine::save_trade_data(const std::string& strategy_name,
                                     const utilities::TradeData& trade) {
    std::scoped_lock lock(db_mutex_);
    if (!conn_) {
        return;
    }
    try {
        std::string ts = datetime_to_str(std::chrono::system_clock::now());
        std::string dir = trade.direction ? utilities::to_string(*trade.direction) : "N/A";
        std::string dt = trade.datetime ? datetime_to_str(*trade.datetime) : "";

        pqxx::work w(*conn_);
        w.exec(pqxx::zview(
                   "INSERT INTO trades (timestamp, strategy_name, tradeid, symbol, exchange, "
                   "orderid, direction, price, volume, datetime) "
                   "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10) "
                   "ON CONFLICT (tradeid) DO UPDATE SET timestamp=$1, strategy_name=$2, symbol=$4, "
                   "exchange=$5, orderid=$6, direction=$7, price=$8, volume=$9, datetime=$10"),
               pqxx::params{ts, strategy_name, trade.tradeid, trade.symbol,
                            utilities::to_string(trade.exchange), trade.orderid, dir, trade.price,
                            trade.volume, dt});
        w.commit();
    } catch (const std::exception& e) {
        write_log(std::string("Failed to save trade: ") + e.what(), ERROR);
    }
}

auto DatabaseEngine::get_all_history_orders() -> std::vector<std::vector<std::string>> {
    std::scoped_lock lock(db_mutex_);
    std::vector<std::vector<std::string>> out;
    if (!conn_) {
        return out;
    }
    try {
        pqxx::work w(*conn_);
        pqxx::result r = w.exec("SELECT * FROM orders ORDER BY timestamp ASC");
        for (auto row : r) {
            std::vector<std::string> vec;
            for (auto&& i : row) {
                vec.push_back(i.is_null() ? "" : std::string(i.c_str()));
            }
            out.push_back(std::move(vec));
        }
        w.commit();
    } catch (const std::exception& e) {
        write_log(std::string("Failed to fetch orders: ") + e.what(), ERROR);
    }
    return out;
}

auto DatabaseEngine::get_all_history_trades() -> std::vector<std::vector<std::string>> {
    std::scoped_lock lock(db_mutex_);
    std::vector<std::vector<std::string>> out;
    if (!conn_) {
        return out;
    }
    try {
        pqxx::work w(*conn_);
        pqxx::result r = w.exec("SELECT * FROM trades ORDER BY timestamp ASC");
        for (auto row : r) {
            std::vector<std::string> vec;
            for (auto&& i : row) {
                vec.push_back(i.is_null() ? "" : std::string(i.c_str()));
            }
            out.push_back(std::move(vec));
        }
        w.commit();
    } catch (const std::exception& e) {
        write_log(std::string("Failed to fetch trades: ") + e.what(), ERROR);
    }
    return out;
}

void DatabaseEngine::wipe_trading_data() {
    std::scoped_lock lock(db_mutex_);
    if (!conn_) {
        return;
    }
    try {
        pqxx::work w(*conn_);
        w.exec("DELETE FROM orders");
        w.exec("DELETE FROM trades");
        w.commit();
        write_log("Trading data wiped successfully", INFO);
    } catch (const std::exception& e) {
        write_log(std::string("Failed to wipe trading data: ") + e.what(), ERROR);
    }
}

void DatabaseEngine::close() {
    std::scoped_lock lock(db_mutex_);
    conn_.reset();
}

} // namespace engines
