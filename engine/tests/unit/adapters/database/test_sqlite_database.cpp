/**
 * The storage on SQLite (SqliteDatabase): orders and fills queued from the event path all reach
 * the file (close() writes what is still queued), the latest state of an order replaces the
 * earlier ones, and contracts, written by the sync or by hand, load back as they were written. Strategies,
 * orders and fills load back as they were, for a restart to resume from: a strategy's holding as
 * last saved, and only what came after it.
 */
#include "adapters/database/sqlite_database.hpp"
#include "model/session_calendar.hpp"
#include "model/utility.hpp"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <gtest/gtest.h>
#include <sqlite3.h>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace std::chrono;


class SqliteDatabaseTest : public ::testing::Test {
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

auto order(int id, model::Status status) -> model::Order {
    model::Order o;
    o.order_id = "OID-" + std::to_string(id);
    o.symbol = "SPY-20991218-C-600";
    o.legs = {{.ratio = 1, .side = model::Side::BUY, .symbol = o.symbol}};
    o.price = 1.25;
    o.quantity = 2;
    o.status = status;
    return o;
}

TEST_F(SqliteDatabaseTest, EveryQueuedOrderAndFillReachesTheFile) {
    constexpr int kOrders = 1000;
    {
        adapters::SqliteDatabase db(nullptr, path_);
        for (int i = 0; i < kOrders; ++i) {
            db.save_order("s1", order(i, model::Status::SUBMITTING));
        }
        db.save_order("s1", order(7, model::Status::FILLED)); // a later state of OID-7
        model::Fill t;
        t.fill_id = "TID-1";
        t.order_id = "OID-7";
        t.symbol = "SPY-20991218-C-600";
        t.price = 1.3;
        t.quantity = 2;
        db.save_fill("s1", t);
        db.close(); // writes what the writer has not written yet
    }
    adapters::SqliteDatabase db(nullptr, path_);
    const auto orders = db.load_orders({}, 0);
    ASSERT_EQ(orders.size(), static_cast<size_t>(kOrders));
    const auto oid7 = std::ranges::find_if(orders, [](const core::SavedOrder& r) {
        return r.record.order_id == "OID-7";
    });
    ASSERT_NE(oid7, orders.end());
    EXPECT_EQ(oid7->record.status, model::Status::FILLED);
    const auto fills = db.load_fills({}, 0);
    ASSERT_EQ(fills.size(), 1U);
    EXPECT_EQ(fills[0].record.fill_id, "TID-1");
    EXPECT_EQ(fills[0].record.order_id, "OID-7");
    EXPECT_TRUE(db.load_orders({"another"}, 0).empty()) << "narrowed to a strategy";
    EXPECT_EQ(db.load_fills({"another", "s1"}, 0).size(), 1U);
}

TEST_F(SqliteDatabaseTest, ContractsLoadBackAndExpiredOptionsAreDropped) {
    adapters::SqliteDatabase(nullptr, path_).close(); // creates the tables
    const std::string common = "'name', 'OPTION', 100, 0.01, 1, 1, 'SPY'";
    sql("INSERT INTO contract_option VALUES ('SPY-20991218-P-600', " + common +
        ", 'PUT', 600, '2099-12-18', 'SPY');"
        // Named some other way (an older sync): kept here, the live engine skips it.
        "INSERT INTO contract_option VALUES ('SPY-20991218-C-700-100-USD-OPT', " +
        common +
        ", 'CALL', 700, '2099-12-18', 'SPY');"
        "INSERT INTO contract_option VALUES ('SPY-20200117-C-300', " +
        common +
        ", 'CALL', 300, '2020-01-17', 'SPY');"
        "INSERT INTO contract_equity VALUES ('SPY', 'SPDR', 'EQUITY', 1, 0.01, 1, 756733,"
        " 'SPY');");

    adapters::SqliteDatabase db(nullptr, path_); // drops the option that expired in 2020
    const std::vector<model::Contract> options = db.load_option_contracts();
    const std::vector<model::Contract> underlyings = db.load_equity_contracts();
    ASSERT_EQ(options.size(), 2U); // the one named some other way too: the live engine skips it
    EXPECT_EQ(options[1].symbol, "SPY-20991218-C-700-100-USD-OPT");
    const model::Contract& put = options[0];
    EXPECT_EQ(put.symbol, "SPY-20991218-P-600");
    EXPECT_EQ(put.option_type, model::OptionType::PUT);
    EXPECT_EQ(put.option_strike, 600);
    EXPECT_EQ(put.option_expiry, model::kNyse.expiry_time(2099y / December / 18));
    EXPECT_EQ(put.trading_class, "SPY");
    ASSERT_EQ(underlyings.size(), 1U);
    EXPECT_EQ(underlyings[0].con_id, 756733);
    EXPECT_EQ(underlyings[0].product, model::Product::EQUITY);
}

TEST_F(SqliteDatabaseTest, SyncedContractsLoadBackAndALaterSyncReplacesThem) {
    model::Contract spy;
    spy.symbol = "SPY";
    spy.name = "SPDR S&P 500 ETF TRUST";
    spy.product = model::Product::EQUITY;
    spy.con_id = 756733;
    spy.trading_class = "SPY";
    model::Contract put;
    put.symbol = "SPY-20991218-P-600";
    put.name = "SPY   991218P00600000";
    put.product = model::Product::OPTION;
    put.multiplier = 100;
    put.con_id = 42;
    put.trading_class = "SPY";
    put.option_type = model::OptionType::PUT;
    put.option_strike = 600;
    put.option_expiry = model::kNyse.expiry_time(2099y / December / 18);
    put.option_underlying = "SPY";
    {
        adapters::SqliteDatabase db(nullptr, path_);
        db.save_contracts(std::vector{spy, put});
        put.con_id = 43; // listed again
        db.save_contracts(std::vector{put});
    }
    adapters::SqliteDatabase db(nullptr, path_);
    const std::vector<model::Contract> options = db.load_option_contracts();
    const std::vector<model::Contract> underlyings = db.load_equity_contracts();
    ASSERT_EQ(options.size(), 1U);
    const model::Contract& loaded = options[0];
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

TEST_F(SqliteDatabaseTest, StrategiesOrdersAndFillsLoadBackForARestart) {
    const sys_seconds at = sys_days{2025y / March / 14} + hours(14) + minutes(31);
    model::Order combo = order(1, model::Status::PARTLY_FILLED);
    combo.symbol = "STRADDLE_SPY_x";
    combo.combo_type = model::ComboType::STRADDLE;
    combo.trading_class = "SPY";
    combo.type = model::OrderType::MARKET;
    combo.filled = 1;
    combo.datetime = at;
    combo.reference = "entry";
    model::Leg call;
    call.con_id = 11;
    call.ratio = 1;
    call.side = model::Side::SELL;
    call.symbol = "SPY-20250314-C-580";
    model::Leg put = call;
    put.con_id = 12;
    put.symbol = "SPY-20250314-P-580";
    combo.legs = {call, put};
    {
        adapters::SqliteDatabase db(nullptr, path_);
        db.save_strategy({.name = "a",
                          .class_name = "RandomBuyStrategy",
                          .portfolio = "SPY",
                          .setting = {{"position_size", 2}, {"rate", 0.1}},
                          .state = model::StrategyState::Added});
        db.save_strategy({.name = "b", .class_name = "X", .portfolio = "SPXW"});
        db.save_strategy({.name = "a",
                          .class_name = "RandomBuyStrategy",
                          .portfolio = "SPY",
                          .setting = {{"position_size", 2}, {"rate", 0.1}},
                          .state = model::StrategyState::Running}); // saved again: keeps its row
        db.delete_strategy("b");
        db.save_order("a", combo);
        for (const char* id : {"T2", "T1"}) { // saved in this order: replayed in it
            model::Fill t;
            t.fill_id = id;
            t.order_id = combo.order_id;
            t.symbol = call.symbol;
            t.side = model::Side::SELL;
            t.price = 2.5;
            t.quantity = 1;
            t.datetime = at;
            db.save_fill("a", t);
        }
        db.close();
    }
    adapters::SqliteDatabase db(nullptr, path_);
    const auto strategies = db.load_strategies();
    ASSERT_EQ(strategies.size(), 1U);
    EXPECT_EQ(strategies[0].name, "a");
    EXPECT_EQ(strategies[0].class_name, "RandomBuyStrategy");
    EXPECT_EQ(strategies[0].portfolio, "SPY");
    EXPECT_EQ(strategies[0].state, model::StrategyState::Running);
    EXPECT_EQ(strategies[0].setting,
              (std::unordered_map<std::string, double>{{"position_size", 2}, {"rate", 0.1}}));

    const auto orders = db.load_orders({}, 0);
    ASSERT_EQ(orders.size(), 1U);
    EXPECT_EQ(orders[0].strategy_name, "a");
    const auto& o = orders[0].record;
    EXPECT_EQ(o.order_id, combo.order_id);
    EXPECT_EQ(o.symbol, combo.symbol);
    EXPECT_TRUE(o.is_combo());
    EXPECT_EQ(o.combo_type, model::ComboType::STRADDLE);
    EXPECT_EQ(o.trading_class, "SPY");
    EXPECT_EQ(o.type, model::OrderType::MARKET);
    EXPECT_EQ(o.status, model::Status::PARTLY_FILLED);
    EXPECT_EQ(o.quantity, 2);
    EXPECT_EQ(o.filled, 1);
    EXPECT_EQ(o.datetime, model::DateTime(at));
    EXPECT_EQ(o.reference, "entry");
    ASSERT_EQ(o.legs.size(), 2U);
    EXPECT_EQ(o.legs[1].symbol, put.symbol);
    EXPECT_EQ(o.legs[1].con_id, 12);
    EXPECT_EQ(o.legs[1].ratio, 1);
    EXPECT_EQ(o.legs[1].side, model::Side::SELL);

    const auto fills = db.load_fills({}, 0);
    ASSERT_EQ(fills.size(), 2U);
    EXPECT_EQ(fills[0].record.fill_id, "T2");
    const auto& t1 = fills[1].record;
    EXPECT_EQ(t1.fill_id, "T1");
    EXPECT_EQ(fills[1].strategy_name, "a");
    EXPECT_EQ(t1.side, model::Side::SELL);
    EXPECT_EQ(t1.datetime, model::DateTime(at));
    db.close();
}

auto order_id_of(const core::SavedOrder& r) -> std::string {
    return r.record.order_id;
}
auto fill_id_of(const core::SavedFill& r) -> std::string {
    return r.record.fill_id;
}

auto fill(const std::string& id, int order) -> model::Fill {
    model::Fill f;
    f.fill_id = id;
    f.order_id = "OID-" + std::to_string(order);
    f.symbol = "SPY-20991218-C-600";
    f.side = model::Side::BUY;
    f.price = 1.25;
    f.quantity = 1;
    return f;
}

TEST_F(SqliteDatabaseTest, ARestartReadsEveryOrderAndFillOfItsStrategiesInTheOrderSaved) {
    {
        adapters::SqliteDatabase db(nullptr, path_);
        db.save_order("a", order(1, model::Status::FILLED));
        db.save_fill("a", fill("T1", 1));
        db.save_order("b", order(9, model::Status::FILLED));
        db.save_fill("b", fill("T9", 9));
        db.save_order("a", order(2, model::Status::WORKING)); // still working
        db.save_order("a", order(1, model::Status::FILLED)); // seen again: replaced
        db.close();
    }
    adapters::SqliteDatabase db(nullptr, path_);
    std::vector<std::string> orders;
    for (const core::SavedOrder& r : db.load_orders({"a"}, 0)) {
        orders.push_back(r.strategy_name + "/" + order_id_of(r));
    }
    EXPECT_EQ(orders, (std::vector<std::string>{"a/OID-2", "a/OID-1"}));
    std::vector<std::string> fills;
    for (const core::SavedFill& r : db.load_fills({"a", "b"}, 0)) {
        fills.push_back(r.strategy_name + "/" + fill_id_of(r));
    }
    EXPECT_EQ(fills, (std::vector<std::string>{"a/T1", "b/T9"}));
    db.close();
}

TEST_F(SqliteDatabaseTest, TheUiReadsTheLatestRecordsAndEmptiesWhatIsQueuedToo) {
    {
        adapters::SqliteDatabase db(nullptr, path_);
        db.save_order("a", order(1, model::Status::FILLED));
        db.save_order("b", order(2, model::Status::FILLED));
        db.save_order("a", order(3, model::Status::FILLED));
        db.save_fill("b", fill("T2", 2));
        db.close();
    }
    adapters::SqliteDatabase db(nullptr, path_);
    std::vector<std::string> latest;
    for (const core::SavedOrder& r : db.load_orders({}, 2)) {
        latest.push_back(order_id_of(r));
    }
    EXPECT_EQ(latest, (std::vector<std::string>{"OID-2", "OID-3"})) << "in the order saved";
    EXPECT_EQ(db.load_orders({"a"}, 1).size(), 1U);
    EXPECT_EQ(db.strategies_with_records(), (std::vector<std::string>{"a", "b"}));

    db.save_fill("a", fill("T3", 3)); // queued: emptied too, not written after
    EXPECT_EQ(db.delete_orders_and_fills(),
              (std::vector<std::pair<std::string, int>>{{"orders", 3}, {"fills", 2}}));
    db.close();
    adapters::SqliteDatabase reopened(nullptr, path_);
    EXPECT_TRUE(reopened.load_fills({}, 0).empty());
    EXPECT_TRUE(reopened.strategies_with_records().empty());
    reopened.close();
}

TEST_F(SqliteDatabaseTest, WithoutAFileTheEngineRunsAndKeepsNothing) {
    adapters::SqliteDatabase db(nullptr, ::testing::TempDir() + "no/such/dir/optra.db");
    db.save_order("s1", order(1, model::Status::SUBMITTING));
    EXPECT_TRUE(db.load_orders({}, 0).empty());
    db.close();
}

} // namespace
