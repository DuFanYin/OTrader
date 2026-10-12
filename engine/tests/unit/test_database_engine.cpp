/**
 * DatabaseEngine on SQLite: orders and trades queued from the event path all reach the file
 * (close() writes what is still queued), the latest state of an order replaces the earlier ones,
 * and contracts, written by the sync or by hand, load back as the engine uses them. Strategies,
 * orders and trades load back as they were, for a restart to resume from: a strategy's holding as
 * last saved, and only what came after it.
 */
#include "adapters/db/engine_db_sqlite.hpp"
#include "model/utility.hpp"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <functional>
#include <gtest/gtest.h>
#include <sqlite3.h>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace std::chrono;

class DatabaseEngineTest : public ::testing::Test {
  protected:
    void SetUp() override {
        path_ = ::testing::TempDir() + "optra_" +
                ::testing::UnitTest::GetInstance()->current_test_info()->name() + ".db";
        remove_files();
    }
    void TearDown() override { remove_files(); }

    void remove_files() const {
        for (const char* suffix : {"", "-wal", "-shm"}) {
            std::filesystem::remove(path_ + suffix);
        }
    }

    /** SQL straight to the file. */
    void sql(const std::string& statement) const {
        sqlite3* db = nullptr;
        ASSERT_EQ(sqlite3_open(path_.c_str(), &db), SQLITE_OK);
        EXPECT_EQ(sqlite3_exec(db, statement.c_str(), nullptr, nullptr, nullptr), SQLITE_OK)
            << sqlite3_errmsg(db);
        sqlite3_close(db);
    }

    /** The one number a query gives, straight from the file. */
    int sql_count(const std::string& query) const {
        sqlite3* db = nullptr;
        sqlite3_open(path_.c_str(), &db);
        sqlite3_stmt* s = nullptr;
        int n = -1;
        if (sqlite3_prepare_v2(db, query.c_str(), -1, &s, nullptr) == SQLITE_OK &&
            sqlite3_step(s) == SQLITE_ROW) {
            n = sqlite3_column_int(s, 0);
        }
        sqlite3_finalize(s);
        sqlite3_close(db);
        return n;
    }

    std::string path_;
};

auto order(int id, model::Status status) -> model::OrderData {
    model::OrderData o;
    o.orderid = "OID-" + std::to_string(id);
    o.symbol = "SPY-20991218-C-600";
    o.direction = model::Direction::LONG;
    o.price = 1.25;
    o.volume = 2;
    o.status = status;
    return o;
}

TEST_F(DatabaseEngineTest, EveryQueuedOrderAndTradeReachesTheFile) {
    constexpr int kOrders = 1000;
    {
        adapters::DatabaseEngine db(nullptr, path_);
        for (int i = 0; i < kOrders; ++i) {
            db.save_order_data("s1", order(i, model::Status::SUBMITTING));
        }
        db.save_order_data("s1", order(7, model::Status::ALLTRADED)); // a later state of OID-7
        model::TradeData t;
        t.tradeid = "TID-1";
        t.orderid = "OID-7";
        t.symbol = "SPY-20991218-C-600";
        t.price = 1.3;
        t.volume = 2;
        db.save_trade_data("s1", t);
        db.close(); // writes what the writer has not written yet
    }
    adapters::DatabaseEngine db(nullptr, path_);
    const auto orders = db.load_orders();
    ASSERT_EQ(orders.size(), static_cast<size_t>(kOrders));
    const auto oid7 = std::ranges::find_if(orders, [](const adapters::SavedRecord& r) {
        return std::get<model::OrderData>(r.record).orderid == "OID-7";
    });
    ASSERT_NE(oid7, orders.end());
    EXPECT_EQ(std::get<model::OrderData>(oid7->record).status, model::Status::ALLTRADED);
    const auto trades = db.load_trades();
    ASSERT_EQ(trades.size(), 1U);
    EXPECT_EQ(std::get<model::TradeData>(trades[0].record).tradeid, "TID-1");
    EXPECT_EQ(std::get<model::TradeData>(trades[0].record).orderid, "OID-7");
    EXPECT_TRUE(db.load_orders({"another"}).empty()) << "narrowed to a strategy";
    EXPECT_EQ(db.load_trades({"another", "s1"}).size(), 1U);
}

TEST_F(DatabaseEngineTest, ContractsLoadBackExpiredAndForeignOptionsDropped) {
    adapters::DatabaseEngine(nullptr, path_).close(); // creates the tables
    const std::string common = "'name', 'OPTION', 100, 0.01, 1, 1, 'SPY'";
    sql("INSERT INTO contract_option VALUES ('SPY-20991218-P-600', " + common +
        ", 'PUT', 600, '2099-12-18', 'SPY');"
        // Named some other way (an older sync): skipped.
        "INSERT INTO contract_option VALUES ('SPY-20991218-C-700-100-USD-OPT', " +
        common +
        ", 'CALL', 700, '2099-12-18', 'SPY');"
        "INSERT INTO contract_option VALUES ('SPY-20200117-C-300', " +
        common +
        ", 'CALL', 300, '2020-01-17', 'SPY');"
        "INSERT INTO contract_equity VALUES ('SPY', 'SPDR', 'EQUITY', 1, 0.01, 1, 756733,"
        " 'SPY');");

    adapters::DatabaseEngine db(nullptr, path_); // drops the option that expired in 2020
    std::vector<model::ContractData> options, underlyings;
    db.load_contracts([&](const model::ContractData& c) { options.push_back(c); },
                      [&](const model::ContractData& c) { underlyings.push_back(c); });
    ASSERT_EQ(options.size(), 1U);
    const model::ContractData& put = options[0];
    EXPECT_EQ(put.symbol, "SPY-20991218-P-600");
    EXPECT_EQ(put.option_type, model::OptionType::PUT);
    EXPECT_EQ(put.option_strike, 600);
    EXPECT_EQ(put.option_expiry, model::option_expiry_time(2099y / December / 18));
    EXPECT_EQ(put.trading_class, "SPY");
    ASSERT_EQ(underlyings.size(), 1U);
    EXPECT_EQ(underlyings[0].con_id, 756733);
    EXPECT_EQ(underlyings[0].product, model::Product::EQUITY);
}

TEST_F(DatabaseEngineTest, SyncedContractsLoadBackAndALaterSyncReplacesThem) {
    model::ContractData spy;
    spy.symbol = "SPY";
    spy.name = "SPDR S&P 500 ETF TRUST";
    spy.product = model::Product::EQUITY;
    spy.con_id = 756733;
    spy.trading_class = "SPY";
    model::ContractData put;
    put.symbol = "SPY-20991218-P-600";
    put.name = "SPY   991218P00600000";
    put.product = model::Product::OPTION;
    put.size = 100;
    put.con_id = 42;
    put.trading_class = "SPY";
    put.option_type = model::OptionType::PUT;
    put.option_strike = 600;
    put.option_expiry = model::option_expiry_time(2099y / December / 18);
    put.option_underlying = "SPY";
    {
        adapters::DatabaseEngine db(nullptr, path_);
        db.save_contracts(std::vector{spy, put});
        put.con_id = 43; // listed again
        db.save_contracts(std::vector{put});
    }
    adapters::DatabaseEngine db(nullptr, path_);
    std::vector<model::ContractData> options, underlyings;
    db.load_contracts([&](const model::ContractData& c) { options.push_back(c); },
                      [&](const model::ContractData& c) { underlyings.push_back(c); });
    ASSERT_EQ(options.size(), 1U);
    const model::ContractData& loaded = options[0];
    EXPECT_EQ(loaded.symbol, put.symbol);
    EXPECT_EQ(loaded.name, put.name);
    EXPECT_EQ(loaded.con_id, 43);
    EXPECT_EQ(loaded.option_type, model::OptionType::PUT);
    EXPECT_EQ(loaded.option_strike, 600);
    EXPECT_EQ(loaded.option_expiry, put.option_expiry);
    EXPECT_EQ(loaded.option_underlying, "SPY");
    ASSERT_EQ(underlyings.size(), 1U);
    EXPECT_EQ(underlyings[0].symbol, "SPY");
    EXPECT_EQ(underlyings[0].name, "SPDR S&P 500 ETF TRUST");
    EXPECT_EQ(underlyings[0].con_id, 756733);
    EXPECT_EQ(underlyings[0].product, model::Product::EQUITY);
}

TEST_F(DatabaseEngineTest, StrategiesOrdersAndTradesLoadBackForARestart) {
    const sys_seconds at = sys_days{2025y / March / 14} + hours(14) + minutes(31);
    model::OrderData combo = order(1, model::Status::PARTTRADED);
    combo.symbol = "STRADDLE_SPY_x";
    combo.is_combo = true;
    combo.combo_type = model::ComboType::STRADDLE;
    combo.trading_class = "SPY";
    combo.type = model::OrderType::MARKET;
    combo.direction = model::Direction::SHORT;
    combo.traded = 1;
    combo.datetime = at;
    combo.reference = "entry";
    model::Leg call;
    call.con_id = 11;
    call.ratio = 1;
    call.direction = model::Direction::SHORT;
    call.symbol = "SPY-20250314-C-580";
    model::Leg put = call;
    put.con_id = 12;
    put.symbol = "SPY-20250314-P-580";
    combo.legs = {call, put};
    {
        adapters::DatabaseEngine db(nullptr, path_);
        db.save_strategy({.name = "a",
                          .class_name = "StraddleTestStrategy",
                          .portfolio = "SPY",
                          .setting = {{"position_size", 2}, {"rate", 0.1}},
                          .state = adapters::StrategyState::Added});
        db.save_strategy({.name = "b", .class_name = "X", .portfolio = "SPXW"});
        db.set_strategy_state("a", adapters::StrategyState::Running);
        db.delete_strategy("b");
        db.save_order_data("a", combo);
        for (const char* id : {"T2", "T1"}) { // saved in this order: replayed in it
            model::TradeData t;
            t.tradeid = id;
            t.orderid = combo.orderid;
            t.symbol = call.symbol;
            t.direction = model::Direction::SHORT;
            t.price = 2.5;
            t.volume = 1;
            t.datetime = at;
            db.save_trade_data("a", t);
        }
        db.close();
    }
    adapters::DatabaseEngine db(nullptr, path_);
    const auto strategies = db.load_strategies();
    ASSERT_EQ(strategies.size(), 1U);
    EXPECT_EQ(strategies[0].name, "a");
    EXPECT_EQ(strategies[0].class_name, "StraddleTestStrategy");
    EXPECT_EQ(strategies[0].portfolio, "SPY");
    EXPECT_EQ(strategies[0].state, adapters::StrategyState::Running);
    EXPECT_EQ(strategies[0].setting,
              (std::unordered_map<std::string, double>{{"position_size", 2}, {"rate", 0.1}}));

    const auto orders = db.load_orders();
    ASSERT_EQ(orders.size(), 1U);
    EXPECT_EQ(orders[0].strategy_name, "a");
    const auto& o = std::get<model::OrderData>(orders[0].record);
    EXPECT_EQ(o.orderid, combo.orderid);
    EXPECT_EQ(o.symbol, combo.symbol);
    EXPECT_TRUE(o.is_combo);
    EXPECT_EQ(o.combo_type, model::ComboType::STRADDLE);
    EXPECT_EQ(o.trading_class, "SPY");
    EXPECT_EQ(o.type, model::OrderType::MARKET);
    EXPECT_EQ(o.direction, model::Direction::SHORT);
    EXPECT_EQ(o.status, model::Status::PARTTRADED);
    EXPECT_EQ(o.volume, 2);
    EXPECT_EQ(o.traded, 1);
    EXPECT_EQ(o.datetime, model::DateTime(at));
    EXPECT_EQ(o.reference, "entry");
    ASSERT_EQ(o.legs.size(), 2U);
    EXPECT_EQ(o.legs[1].symbol, put.symbol);
    EXPECT_EQ(o.legs[1].con_id, 12);
    EXPECT_EQ(o.legs[1].ratio, 1);
    EXPECT_EQ(o.legs[1].direction, model::Direction::SHORT);

    const auto trades = db.load_trades();
    ASSERT_EQ(trades.size(), 2U);
    EXPECT_EQ(std::get<model::TradeData>(trades[0].record).tradeid, "T2");
    const auto& t1 = std::get<model::TradeData>(trades[1].record);
    EXPECT_EQ(t1.tradeid, "T1");
    EXPECT_EQ(trades[1].strategy_name, "a");
    EXPECT_EQ(t1.direction, model::Direction::SHORT);
    EXPECT_EQ(t1.datetime, model::DateTime(at));
    db.close();
}

TEST_F(DatabaseEngineTest, AnOlderOrdersTableGainsTheComboType) {
    sql("CREATE TABLE orders (timestamp TEXT NOT NULL, strategy_name TEXT NOT NULL,"
        " orderid TEXT PRIMARY KEY, symbol TEXT NOT NULL, exchange TEXT NOT NULL,"
        " trading_class TEXT, type TEXT NOT NULL, direction TEXT, price REAL NOT NULL,"
        " volume REAL NOT NULL, traded REAL NOT NULL, status TEXT NOT NULL, datetime TEXT,"
        " reference TEXT, is_combo INTEGER, legs_info TEXT)");
    {
        adapters::DatabaseEngine db(nullptr, path_);
        db.save_order_data("s1", order(1, model::Status::ALLTRADED));
        db.close();
    }
    adapters::DatabaseEngine db(nullptr, path_);
    const auto orders = db.load_orders();
    ASSERT_EQ(orders.size(), 1U);
    const auto& o = std::get<model::OrderData>(orders[0].record);
    EXPECT_EQ(o.status, model::Status::ALLTRADED);
    EXPECT_FALSE(o.is_combo);
    EXPECT_EQ(o.combo_type, model::ComboType::CUSTOM); // a single contract's: none saved
    db.close();
}

/** The order a SavedRecord holds. */
auto orderid_of(const adapters::SavedRecord& r) -> std::string {
    return std::get<model::OrderData>(r.record).orderid;
}
auto tradeid_of(const adapters::SavedRecord& r) -> std::string {
    return std::get<model::TradeData>(r.record).tradeid;
}

auto trade(const std::string& id, int order) -> model::TradeData {
    model::TradeData t;
    t.tradeid = id;
    t.orderid = "OID-" + std::to_string(order);
    t.symbol = "SPY-20991218-C-600";
    t.direction = model::Direction::LONG;
    t.price = 1.25;
    t.volume = 1;
    return t;
}

TEST_F(DatabaseEngineTest, ARestartReadsEveryOrderAndTradeOfItsStrategiesInTheOrderSaved) {
    {
        adapters::DatabaseEngine db(nullptr, path_);
        db.save_order_data("a", order(1, model::Status::ALLTRADED));
        db.save_trade_data("a", trade("T1", 1));
        db.save_order_data("b", order(9, model::Status::ALLTRADED));
        db.save_trade_data("b", trade("T9", 9));
        db.save_order_data("a", order(2, model::Status::NOTTRADED)); // still working
        db.save_order_data("a", order(1, model::Status::ALLTRADED)); // seen again: replaced
        db.close();
    }
    adapters::DatabaseEngine db(nullptr, path_);
    std::vector<std::string> orders;
    for (const adapters::SavedRecord& r : db.load_orders({"a"})) {
        orders.push_back(r.strategy_name + "/" + orderid_of(r));
    }
    EXPECT_EQ(orders, (std::vector<std::string>{"a/OID-2", "a/OID-1"}));
    std::vector<std::string> trades;
    for (const adapters::SavedRecord& r : db.load_trades({"a", "b"})) {
        trades.push_back(r.strategy_name + "/" + tradeid_of(r));
    }
    EXPECT_EQ(trades, (std::vector<std::string>{"a/T1", "b/T9"}));
    db.close();
}

TEST_F(DatabaseEngineTest, AnOlderFileLosesItsHoldingsAndTheColumnsNoLongerKept) {
    sql("CREATE TABLE holdings (strategy_name TEXT PRIMARY KEY, holding BLOB NOT NULL)");
    sql("CREATE TABLE trades (timestamp TEXT NOT NULL, strategy_name TEXT NOT NULL,"
        " tradeid TEXT PRIMARY KEY, symbol TEXT NOT NULL, exchange TEXT NOT NULL,"
        " orderid TEXT NOT NULL, direction TEXT, price REAL NOT NULL, volume REAL NOT NULL,"
        " datetime TEXT)");
    {
        adapters::DatabaseEngine db(nullptr, path_);
        db.save_trade_data("a", trade("T1", 1)); // no exchange to give: the column is gone
        db.close();
    }
    adapters::DatabaseEngine db(nullptr, path_);
    ASSERT_EQ(db.load_trades({"a"}).size(), 1U);
    db.close();
    EXPECT_EQ(sql_count("SELECT COUNT(*) FROM sqlite_master WHERE name = 'holdings'"), 0);
}

TEST_F(DatabaseEngineTest, TheUiReadsTheLatestRecordsAndEmptiesWhatIsQueuedToo) {
    {
        adapters::DatabaseEngine db(nullptr, path_);
        db.save_order_data("a", order(1, model::Status::ALLTRADED));
        db.save_order_data("b", order(2, model::Status::ALLTRADED));
        db.save_order_data("a", order(3, model::Status::ALLTRADED));
        db.save_trade_data("b", trade("T2", 2));
        db.close();
    }
    adapters::DatabaseEngine db(nullptr, path_);
    std::vector<std::string> latest;
    for (const adapters::SavedRecord& r : db.load_orders({}, 2)) {
        latest.push_back(orderid_of(r));
    }
    EXPECT_EQ(latest, (std::vector<std::string>{"OID-2", "OID-3"})) << "in the order saved";
    EXPECT_EQ(db.load_orders({"a"}, 1).size(), 1U);
    EXPECT_EQ(db.strategies_with_records(), (std::vector<std::string>{"a", "b"}));

    db.save_trade_data("a", trade("T3", 3)); // queued: emptied too, not written after
    EXPECT_EQ(db.delete_orders_and_trades(),
              (std::vector<std::pair<std::string, int>>{{"orders", 3}, {"trades", 2}}));
    db.close();
    adapters::DatabaseEngine reopened(nullptr, path_);
    EXPECT_TRUE(reopened.load_trades().empty());
    EXPECT_TRUE(reopened.strategies_with_records().empty());
    reopened.close();
}

TEST_F(DatabaseEngineTest, WithoutAFileTheEngineRunsAndKeepsNothing) {
    adapters::DatabaseEngine db(nullptr, ::testing::TempDir() + "no/such/dir/optra.db");
    db.save_order_data("s1", order(1, model::Status::SUBMITTING));
    EXPECT_TRUE(db.load_orders().empty());
    db.close();
}

} // namespace
