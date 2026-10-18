/**
 * Integration tests against an IB paper account: the live engine in process, connected to IB
 * Gateway (or TWS) logged in to a paper account. Skipped unless OPTRA_IB_PAPER_TESTS=1:
 *
 *   OPTRA_IB_PAPER_TESTS=1 IB_PORT=4002 IB_ACCOUNT=DU... ctest -L ib_paper --output-on-failure
 *
 * IB_HOST (default 127.0.0.1), IB_PORT and IB_ACCOUNT (the paper account) as for the engine.
 * OPTRA_DB names a database with SPY's contracts (`entry_live --sync SPY`; default
 * data/optra.db). Each test works on a copy: a fresh one, but the order test's, which it keeps in
 * the build directory so that an order a failed run left working is cancelled by the next. With
 * IBC_COMMAND_PORT (IBC's command server on 127.0.0.1) the order test also has IBC restart IB
 * Gateway. The tests connect as IB client ids 41 to 54: no other session may use them.
 *
 * Nothing fills: the one order is BUY 1 SPY at 100, and the strategies run without prices (market
 * data type 1, which a paper account does not get); but ComboFillsAreBookedAsLegs, which trades at
 * market and runs only with OPTRA_IB_PAPER_FILLS=1.
 */

#include "tests/read_main.hpp"
#include "core/bookkeeping/bookkeeping_engine.hpp"
#include "core/strategy/strategy_engine.hpp"
#include "assembly/live.hpp"
#include "model/order_builder.hpp"
#include "model/symbol.hpp"
#include "model/utility.hpp"
#include "strategy/strategy_registry.hpp"
#include "strategy/strategy.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sqlite3.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <format>
#include <functional>
#include <tuple>
#include <gtest/gtest.h>
#include <iostream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using model::Status;

constexpr double kPrice = 100.0; // far below SPY: the order works and never fills
constexpr auto kWait = 90s;
constexpr auto kRestartWait = 180s; // IB Gateway closes about a minute after IBC is asked
constexpr auto kSteady = 15s;       // connected this long: the reconnect's catch-up has run
// The control, never buying: no option costs under 1 USD. A strategy that runs and subscribes.
const std::unordered_map<std::string, double> kQuiet{{"premium_budget", 1}};

auto env(const char* name) -> std::string {
    const char* v = std::getenv(name);
    return v != nullptr ? v : "";
}

/** The database the tests copy: OPTRA_DB as it was before any test set it to a copy. */
auto source_db() -> const std::string& {
    static const std::string source = env("OPTRA_DB").empty() ? OPTRA_DEFAULT_DB
                                                                : env("OPTRA_DB");
    return source;
}

/** A file in the build directory. */
auto build_file(std::string_view name) -> std::string {
    return std::format("{}/{}", OPTRA_IB_PAPER_DIR, name);
}

/** A copy of the source database at `to`, through SQLite's backup (a file copy misses what is
 * still in the WAL); with no_records, its orders and fills left out: IB reports this client
 * id's fills of today at connect, and an order id of the source's (another client's) would take
 * them. */
void copy_db(const std::string& to, bool no_records = false) {
    for (const char* suffix : {"", "-wal", "-shm"}) {
        std::remove((to + suffix).c_str());
    }
    sqlite3* src = nullptr;
    sqlite3* dst = nullptr;
    int rc = sqlite3_open_v2(source_db().c_str(), &src, SQLITE_OPEN_READONLY, nullptr);
    if (rc == SQLITE_OK) {
        rc = sqlite3_open(to.c_str(), &dst);
    }
    if (rc == SQLITE_OK) {
        sqlite3_backup* backup = sqlite3_backup_init(dst, "main", src, "main");
        rc = backup != nullptr ? sqlite3_backup_step(backup, -1) : sqlite3_errcode(dst);
        sqlite3_backup_finish(backup);
        rc = rc == SQLITE_DONE ? SQLITE_OK : rc;
    }
    if (rc == SQLITE_OK && no_records) {
        rc = sqlite3_exec(dst, "DELETE FROM orders; DELETE FROM fills;", nullptr, nullptr,
                          nullptr);
    }
    sqlite3_close(dst);
    sqlite3_close(src);
    if (rc != SQLITE_OK) {
        throw std::runtime_error(
            std::format("Cannot copy {} to {}: {}", source_db(), to, sqlite3_errstr(rc)));
    }
}

/** The next engine built works on `db`, connects as client_id (quotes: client_id + 1) and asks
 * for live prices only. */
void use(const std::string& db, int client_id) {
    setenv("OPTRA_DB", db.c_str(), 1);
    setenv("IB_CLIENT_ID", std::to_string(client_id).c_str(), 1);
    setenv("IB_MARKET_DATA_TYPE", "1", 1);
}

auto positions_check(core::Main& engine) -> std::pair<std::string, std::string> {
    return engine.event_loop()->on_event_thread(
        [&] { return engine.bookkeeping_engine()->broker_check(); });
}

auto started(core::Main& engine, const std::string& name) -> bool {
    return engine.event_loop()->on_event_thread([&] {
        const model::StrategyRecord* s = engine.strategy_engine()->find_record(name);
        return s != nullptr && s->state == model::StrategyState::Running;
    });
}

/** The order's status as the engine has it. */
auto status_of(core::Main& engine, const std::string& order_id) -> std::optional<Status> {
    return engine.event_loop()->on_event_thread([&]() -> std::optional<Status> {
        const model::Order* o = engine.bookkeeping_engine()->find_order(order_id);
        return o != nullptr ? std::optional(o->status) : std::nullopt;
    });
}

/** Wait (up to kWait) until the order's status satisfies done; prints each change. */
auto wait_for(core::Main& engine, const std::string& order_id,
              const std::function<bool(Status)>& done) -> std::optional<Status> {
    std::optional<Status> last;
    const auto until = std::chrono::steady_clock::now() + kWait;
    while (std::chrono::steady_clock::now() < until) {
        const std::optional<Status> now = status_of(engine, order_id);
        if (now != last && now) {
            std::cout << "        order " << order_id << ": " << model::to_string(*now) << "\n";
        }
        last = now;
        if (now && done(*now)) {
            return now;
        }
        std::this_thread::sleep_for(200ms);
    }
    return last;
}

auto working(Status s) -> bool { return s == Status::WORKING || s == Status::PARTLY_FILLED; }

auto working(const std::optional<Status>& s) -> bool { return s && working(*s); }

/** Ask IBC, through its command server on 127.0.0.1:port, to restart IB Gateway. */
auto ask_ibc_to_restart(int port) -> bool {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return false;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<std::uint16_t>(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const std::string command = "RESTART\n";
    bool sent = ::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) == 0 &&
                ::send(fd, command.data(), command.size(), 0) ==
                    static_cast<ssize_t>(command.size());
    if (sent) {
        char reply[256];
        const timeval timeout{.tv_sec = 5, .tv_usec = 0};
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
        sent = ::recv(fd, reply, sizeof reply, 0) > 0; // "OK RESTART in progress"
    }
    ::close(fd);
    return sent;
}

/** Wait (up to kRestartWait) until the connection to IB is down, or has been up for kSteady. */
auto wait_until(core::Main& engine, bool up) -> bool {
    const auto until = std::chrono::steady_clock::now() + kRestartWait;
    auto up_since = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() < until) {
        const bool connected = engine.gateway()->is_connected();
        if (!up && !connected) {
            return true;
        }
        if (!connected) {
            up_since = std::chrono::steady_clock::now();
        } else if (up && std::chrono::steady_clock::now() - up_since >= kSteady) {
            return true;
        }
        std::this_thread::sleep_for(500ms);
    }
    return false;
}

class IbPaper : public ::testing::Test {
  protected:
    void SetUp() override {
        if (env("OPTRA_IB_PAPER_TESTS") != "1") {
            GTEST_SKIP() << "against an IB paper account: set OPTRA_IB_PAPER_TESTS=1, IB_PORT "
                            "and IB_ACCOUNT";
        }
        paper_ = env("IB_ACCOUNT");
        ASSERT_FALSE(paper_.empty()) << "Set IB_ACCOUNT to the paper account";
        // The tests work on copies of the paper engine's database: they restore its strategies,
        // installed for the paper account, as it does.
        [[maybe_unused]] static const bool paper_strategies = [] {
            strategy::StrategyRegistry::load(std::filesystem::path(OPTRA_STRATEGY_LIBRARIES) /
                                             "paper");
            return true;
        }();
    }
    void TearDown() override {
        if (!paper_.empty()) {
            setenv("IB_ACCOUNT", paper_.c_str(), 1);
        }
    }

    std::string paper_;
};

TEST_F(IbPaper, OnlyTheAccountInIbAccountConnects) {
    const std::string db = build_file("ib_paper_account.db");
    copy_db(db);
    use(db, 41);
    const std::vector<std::pair<std::string, std::optional<std::string>>> refused{
        {"another paper account", "DU0000000"},
        {"a live-looking account", "U0000000"},
        {"no account", std::nullopt},
    };
    {
        const auto made = live::make_main();
        core::Main& engine = *made;
        EXPECT_NO_THROW(engine.connect()) << "IB_ACCOUNT";
        engine.gateway()->disconnect();
        engine.close();
    }
    for (const auto& [what, account] : refused) {
        if (account) {
            setenv("IB_ACCOUNT", account->c_str(), 1);
        } else {
            unsetenv("IB_ACCOUNT");
        }
        const auto made = live::make_main();
        core::Main& engine = *made;
        EXPECT_THROW(engine.connect(), std::exception) << what;
        engine.gateway()->disconnect();
        engine.close();
    }
}

TEST_F(IbPaper, AResumedEngineStartsTheStrategiesThatWereRunning) {
    // A database of its own, empty: the contracts come in as the run starts.
    const std::string db = build_file("ib_paper_resume.db");
    for (const char* suffix : {"", "-wal", "-shm"}) {
        std::filesystem::remove(db + suffix);
    }
    use(db, 43);
    const std::vector<live::Synced> spy{{"SPY", 7}};
    std::string name;
    {
        const auto made = live::make_main();
        core::Main& engine = *made;
        live::TradingDays days(engine);
        name = engine.add_strategy("RandomBuyStrategy", "SPY", kQuiet);
        days.start(spy, /*resume=*/false);
        EXPECT_EQ(positions_check(engine).first, "agree") << positions_check(engine).second;
        EXPECT_TRUE(engine.market_data()->running());
        engine.init_strategy(name);
        engine.start_strategy(name);
        ASSERT_TRUE(started(engine, name));
        days.stop(); // the run stopped, then what the engine owns: the strategy not stopped
        engine.close();
    }
    const auto made = live::make_main();
    core::Main& engine = *made;
    live::TradingDays days(engine);
    EXPECT_FALSE(started(engine, name)) << "back stopped, until the run starts";
    days.start(spy, /*resume=*/true);
    EXPECT_EQ(positions_check(engine).first, "agree") << positions_check(engine).second;
    EXPECT_TRUE(engine.market_data()->running());
    EXPECT_TRUE(started(engine, name));
    const std::size_t sent =
        engine.event_loop()->on_event_thread(
            [&] { return engine.bookkeeping_engine()->order_count(); });
    EXPECT_EQ(sent, 0U) << "it sent nothing: no prices";
    engine.stop_strategy(name);
    days.stop();
    engine.close();
}

TEST_F(IbPaper, ADaysRollSyncsTheContractsInWhileTheEngineAndItsStrategiesRunOn) {
    // A database of its own, empty: the contracts all come in with the roll, and no position
    // the account holds besides is the engine's.
    const std::string db = build_file("ib_paper_roll.db");
    for (const char* suffix : {"", "-wal", "-shm"}) {
        std::filesystem::remove(db + suffix);
    }
    use(db, 49);
    const auto made = live::make_main();
    core::Main& engine = *made;
    const std::string name = engine.add_strategy("RandomBuyStrategy", "SPY", kQuiet);
    engine.connect();
    engine.market_data()->start();
    engine.init_strategy(name);
    engine.start_strategy(name);
    ASSERT_TRUE(started(engine, name));

    const std::vector<std::pair<std::string, int>> spy{{"SPY", 7}};
    ASSERT_NO_THROW(engine.roll_day(spy));
    EXPECT_TRUE(engine.market_data()->running()) << "streaming again, with the day's contracts";
    EXPECT_TRUE(started(engine, name)) << "the strategy runs on";
    const auto [options, expired] = engine.event_loop()->on_event_thread([&] {
        const model::Portfolio* p = engine.find_portfolio("SPY");
        const auto today = model::new_york_date(std::chrono::system_clock::now());
        const model::Listing& listing = p->listing();
        std::size_t gone = 0;
        for (std::size_t c = 0; c < listing.size(); ++c) {
            const model::DateTime expiry = listing.chains()[listing.chain_of(c)].expiry;
            gone += model::new_york_date(expiry) < today ? 1 : 0;
        }
        return std::pair{p->listing().size(), gone};
    });
    EXPECT_GT(options, 0U);
    EXPECT_EQ(expired, 0U) << "nothing expired before today is left, nothing being held";
    engine.stop_strategy(name);
    engine.gateway()->disconnect();
    engine.close();
}

TEST_F(IbPaper, AnOrderSurvivesRestartsOfTheEngineAndOfIbGateway) {
    // Kept across runs: a run that failed left its order working, and this one cancels it.
    const std::string db = build_file("ib_paper_orders.db");
    if (!std::filesystem::exists(db)) {
        copy_db(db);
    }
    use(db, 45);
    const std::string strategy =
        std::format("PaperOrderTest_{:%Y%m%d%H%M%S}",
                    std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
    std::string order_id;

    std::cout << "[1] Send a limit order far from the market\n";
    {
        const auto made = live::make_main();
        core::Main& engine = *made;
        engine.add_strategy("RandomBuyStrategy", "SPY", {}, strategy);
        engine.connect();
        order_id = engine.event_loop()->on_event_thread([&] {
            const std::array leg{
                model::contract_leg(*engine.find_contract("SPY"), model::Side::BUY)};
            return engine.send_order(
                optra_tests::record_of(engine, strategy),
                model::order_request(leg, kPrice, 1, model::OrderType::LIMIT));
        });
        ASSERT_FALSE(order_id.empty()) << "the gateway took no order";
        EXPECT_TRUE(working(wait_for(engine, order_id, [](Status s) { return working(s); })))
            << "IB has it working";
        std::cout << "[2] Stop the engine; the order stays at IB\n";
        engine.gateway()->disconnect();
        engine.close();
    }

    std::cout << "[3] A new engine on the same database\n";
    const auto made = live::make_main();
    core::Main& engine = *made;
    EXPECT_TRUE(engine.event_loop()->on_event_thread([&] {
        return engine.strategy_engine()->find_strategy(strategy) != nullptr;
    })) << "the strategy is back";
    EXPECT_TRUE(working(status_of(engine, order_id))) << "its order is back, working";
    engine.connect();
    std::this_thread::sleep_for(3s); // the catch-up's updates, if any, have come in
    EXPECT_TRUE(working(status_of(engine, order_id))) << "still working after catching up";

    if (const std::string port = env("IBC_COMMAND_PORT"); !port.empty()) {
        std::cout << "[4] IBC restarts IB Gateway; the engine reconnects and catches up\n";
        EXPECT_TRUE(ask_ibc_to_restart(std::stoi(port))) << "IBC took the restart";
        EXPECT_TRUE(wait_until(engine, false)) << "the connection dropped";
        EXPECT_TRUE(wait_until(engine, true)) << "the connection is back";
        EXPECT_TRUE(working(status_of(engine, order_id))) << "still working after the reconnect";
    } else {
        std::cout << "[4] IB Gateway's restart left out: no IBC_COMMAND_PORT\n";
    }

    std::cout << "[5] Cancel it, and whatever earlier runs left working\n";
    const std::vector<std::string> leftover = engine.event_loop()->on_event_thread([&] {
        std::vector<std::string> ids;
        engine.bookkeeping_engine()->for_each_active_order({}, [&](const model::Order& o) {
            const std::string& owner =
                engine.bookkeeping_engine()->strategy_of(o.order_id);
            if (o.order_id != order_id && owner.starts_with("PaperOrderTest_")) {
                ids.push_back(o.order_id);
            }
        });
        return ids;
    });
    engine.event_loop()->on_event_thread([&] {
        for (const std::string& id : leftover) {
            engine.cancel_order({.order_id = id, .symbol = "SPY"});
        }
        engine.cancel_order({.order_id = order_id, .symbol = "SPY"});
    });
    EXPECT_EQ(wait_for(engine, order_id, [](Status s) { return !working(s); }), Status::CANCELLED)
        << "IB cancelled it";
    for (const std::string& id : leftover) {
        EXPECT_EQ(wait_for(engine, id, [](Status s) { return !working(s); }), Status::CANCELLED)
            << "IB cancelled an earlier run's order " << id;
    }
    engine.gateway()->disconnect();
    engine.close();
}

TEST_F(IbPaper, AnOrderIbRefusesIsRejectedWithItsReason) {
    const std::string db = build_file("ib_paper_reject.db");
    copy_db(db, true);
    use(db, 51);
    const auto made = live::make_main();
    core::Main& engine = *made;
    const std::string strategy = engine.add_strategy("RandomBuyStrategy", "SPY", kQuiet);
    engine.connect();
    // No quotes here (market data is not started; closed days have none): the age check would
    // refuse the order before IB sees it.
    engine.set_risk_settings({{"max_quote_age_seconds", 0}});
    // A price off SPY's tick (0.01): IB refuses it (110) before it ever works.
    const std::string order_id = engine.event_loop()->on_event_thread([&] {
        const std::array leg{model::contract_leg(*engine.find_contract("SPY"), model::Side::BUY)};
        return engine.send_order(
            optra_tests::record_of(engine, strategy),
            model::order_request(leg, kPrice + 0.001, 1, model::OrderType::LIMIT));
    });
    ASSERT_FALSE(order_id.empty()) << "the gateway took no order";
    const std::optional<Status> status =
        wait_for(engine, order_id, [](Status s) { return s == Status::REJECTED || working(s); });
    if (status && working(*status)) { // the market closed: IB holds the order for the open, its price unchecked
        // Cancelled at IB, with any an earlier run of this test left working there: this test's
        // client placed them all, under lower ids (one IB no longer has answers that it has not).
        for (int id = 1; id < std::stoi(order_id); ++id) {
            engine.gateway()->cancel_order({.order_id = std::to_string(id), .symbol = "SPY"});
        }
        engine.event_loop()->on_event_thread(
            [&] { engine.cancel_order({.order_id = order_id, .symbol = "SPY"}); });
        EXPECT_EQ(wait_for(engine, order_id, [](Status s) { return !working(s); }),
                  Status::CANCELLED);
        std::this_thread::sleep_for(2s); // IB's answers to the earlier ones
        engine.gateway()->disconnect();
        engine.close();
        GTEST_SKIP() << "the market is closed: IB checks the price only when it sends the order on";
    }
    EXPECT_EQ(status, Status::REJECTED) << "IB refused it and the engine says so";
    engine.gateway()->disconnect();
    engine.close();
}

TEST_F(IbPaper, AnExpiryTheEngineDidNotSeeSettlesAtIbsClose) {
    // An option held through a close the engine was not running for: asked of IB, its close
    // settles it (a call far out of the money: worthless, no shares delivered).
    const std::string db = build_file("ib_paper_settle.db");
    copy_db(db, true);
    use(db, 53);
    auto expiry = model::new_york_date(std::chrono::system_clock::now());
    do {
        expiry = std::chrono::sys_days{expiry} - std::chrono::days{1};
    } while (!model::is_trading_day(expiry));
    const std::string option = model::option_symbol(
        {.root = "SPY", .expiry = expiry, .type = model::OptionType::CALL, .strike = 2000});
    std::string strategy;
    {
        const auto made = live::make_main();
        core::Main& engine = *made;
        strategy = engine.add_strategy("RandomBuyStrategy", "SPY", kQuiet);
        const auto [order, fill] = model::filled_order(
            "settle-test-1", option, model::Side::BUY, 1, 0.01,
            model::new_york_time(expiry, std::chrono::hours{10}), "test: held through the close");
        engine.database()->save_order(strategy, order);
        engine.database()->save_fill(strategy, fill);
        engine.close();
    }
    const auto made = live::make_main();
    core::Main& engine = *made;
    auto held = [&] {
        return engine.event_loop()->on_event_thread([&] {
            const model::StrategyHolding* h = engine.bookkeeping_engine()->find_holding(strategy);
            return h != nullptr && h->options.contains(option) &&
                   h->options.at(option).quantity != 0;
        });
    };
    ASSERT_TRUE(held()) << "the fill is rebooked";
    engine.connect();
    engine.market_data()->start(); // the close is asked on the quotes' connection, as each day starts
    const auto until = std::chrono::steady_clock::now() + kWait;
    while (held() && std::chrono::steady_clock::now() < until) {
        std::this_thread::sleep_for(500ms);
    }
    EXPECT_FALSE(held()) << "settled at IB's close of " << expiry;
    const auto unsettled = engine.event_loop()->on_event_thread(
        [&] { return engine.bookkeeping_engine()->unsettled_options(); });
    for (const auto& [name, options] : unsettled) {
        ADD_FAILURE() << name << " has options not settled: " << options.size() << ", "
                      << options.front();
    }
    engine.gateway()->disconnect();
    engine.close();
}

/** Combo fills at IB are booked as leg fills (the BAG execution is not booked), and the holding is
 * flat after selling back. An SPXW call spread is bought and sold back, then one call is bought
 * and sold as a single-leg combo, all at market. It trades, so it runs only with
 * OPTRA_IB_PAPER_FILLS=1 during market hours. It keeps its own database copy across runs, with
 * risk limits that allow a spread (the paper account's limits refuse a short leg): what an earlier
 * run left working is cancelled and what it left held is closed (when the account holds it),
 * first; an order that does not fill is cancelled and the rest skipped (IB refuses an order on the
 * other side of one working); and this run's holding is closed at the end, whatever happened. */
TEST_F(IbPaper, ComboFillsAreBookedAsLegs) {
    if (env("OPTRA_IB_PAPER_FILLS") != "1") {
        GTEST_SKIP() << "it fills: set OPTRA_IB_PAPER_FILLS=1 during market hours";
    }
    const std::string db = build_file("ib_paper_fills.db");
    if (!std::filesystem::exists(db)) {
        copy_db(db, /*no_records=*/true);
    }
    use(db, 47);
    setenv("IB_MARKET_DATA_TYPE", "4", 1); // delayed quotes: enough to find the money
    const std::string strategy =
        std::format("PaperFillTest_{:%Y%m%d%H%M%S}",
                    std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));

    const auto made = live::make_main();
    core::Main& engine = *made;
    if (env("OPTRA_IB_PAPER_RAW") == "1") {
        // IB's callbacks as they come (IB orderStatus, ...).
        engine.log_engine()->set_level(utilities::DEBUG);
    }
    engine.add_strategy("RandomBuyStrategy", "SPXW", {}, strategy); // never started
    engine.set_risk_settings(
        {{"short_options", 1}, {"max_order_premium", 0}, {"market_max_spread", 0}});
    engine.connect();
    std::this_thread::sleep_for(3s); // the catch-up's updates, if any, have come in

    // Cancel the active orders and close the holdings, at market, of the strategies `whose` names.
    const auto clean_up = [&](const std::function<bool(const std::string&)>& whose) {
        const auto [active_orders, held] = engine.event_loop()->on_event_thread([&] {
            std::vector<model::Order> orders;
            engine.bookkeeping_engine()->for_each_active_order({}, [&](const model::Order& o) {
                if (whose(engine.bookkeeping_engine()->strategy_of(o.order_id))) {
                    orders.push_back(o);
                }
            });
            std::vector<std::tuple<std::string, std::string, double>> open;
            for (const model::StrategyRecord& r : optra_tests::strategies(engine)) {
                const model::StrategyHolding* h = engine.bookkeeping_engine()->find_holding(r.name);
                if (whose(r.name) && h != nullptr) {
                    h->for_each_open_option([&](const model::OptionPosition& p) {
                        open.emplace_back(r.name, p.symbol, p.quantity);
                    });
                }
            }
            return std::pair{orders, open};
        });
        for (const model::Order& o : active_orders) {
            std::cout << "        cancel " << o.order_id << " " << o.symbol << "\n";
            engine.event_loop()->on_event_thread(
                [&] { engine.cancel_order({.order_id = o.order_id, .symbol = o.symbol}); });
            EXPECT_EQ(wait_for(engine, o.order_id, [](Status s) { return !working(s); }),
                      Status::CANCELLED)
                << "IB cancelled " << o.order_id;
        }
        // Closed only when the account holds it: a database that missed a fill must not open one.
        std::map<std::string, double> account;
        if (!held.empty()) {
            for (const core::AccountPosition& p : engine.gateway()->query_account().get().positions) {
                account[p.symbol] += p.quantity;
            }
        }
        for (const auto& [owner, symbol, quantity] : held) {
            if (quantity == 0) {
                continue;
            }
            const double at_ib = account[symbol];
            if (at_ib * quantity <= 0 || std::abs(at_ib) < std::abs(quantity)) {
                ADD_FAILURE() << owner << " holds " << quantity << " " << symbol
                              << " but the account " << at_ib << ": not closed, set it right";
                continue;
            }
            std::cout << "        close " << owner << " " << symbol << " " << quantity << "\n";
            const std::string id = engine.event_loop()->on_event_thread([&] {
                const std::array leg{model::contract_leg(
                    *engine.find_contract(symbol),
                    quantity > 0 ? model::Side::SELL : model::Side::BUY)};
                return engine.send_order(
                    optra_tests::record_of(engine, owner),
                    model::order_request(leg, 0.0, std::abs(quantity), model::OrderType::MARKET));
            });
            EXPECT_FALSE(id.empty()) << "a closing order for " << symbol << " went";
            if (!id.empty()) {
                EXPECT_EQ(wait_for(engine, id, [](Status s) { return !working(s); }),
                          Status::FILLED)
                    << symbol << " closed";
            }
        }
    };
    std::cout << "[earlier runs]\n";
    clean_up([&](const std::string& owner) {
        return owner.starts_with("PaperFillTest_") && owner != strategy;
    });

    // The first expiry after today, and its strikes nearest the money.
    const std::string today = std::format("SPXW_{:%Y%m%d}", std::chrono::system_clock::now());
    const std::string chain = engine.event_loop()->on_event_thread([&] {
        std::string best;
        const model::Listing& listing = engine.find_portfolio("SPXW")->listing();
        for (const model::Listing::Chain& listed : listing.chains()) {
            if (listed.symbol > today && (best.empty() || listed.symbol < best)) {
                best = listed.symbol;
            }
        }
        return best;
    });
    ASSERT_FALSE(chain.empty()) << "no SPXW expiry after today in the database";
    engine.market_data()->subscribe_chains(strategy, std::vector{chain});
    engine.market_data()->start();
    double forward = 0;
    for (auto until = std::chrono::steady_clock::now() + 90s;
         forward <= 0 && std::chrono::steady_clock::now() < until;
         std::this_thread::sleep_for(500ms)) {
        forward = engine.event_loop()->on_event_thread([&] {
            return engine.find_portfolio("SPXW")->chain(chain)->forward();
        });
    }
    ASSERT_GT(forward, 0) << "no quotes for " << chain;
    const auto [low, high] = engine.event_loop()->on_event_thread([&] {
        // The calls at the strikes either side of the forward.
        const model::Chain c = *engine.find_portfolio("SPXW")->chain(chain);
        const std::span<const double> strikes = c.strikes();
        const auto it = std::ranges::lower_bound(strikes, forward);
        const auto call_at = [&](auto at) -> std::string {
            const std::optional<model::Option> o = c.call(*at);
            return o ? o->symbol() : std::string{};
        };
        return std::pair{it != strikes.begin() ? call_at(std::prev(it)) : std::string{},
                         it != strikes.end() ? call_at(it) : std::string{}};
    });
    ASSERT_TRUE(!low.empty() && !high.empty());
    std::cout << "        " << chain << " forward " << forward << ": " << low << " / " << high
              << "\n";

    const model::ComboGetContractFn find_contract = [&](const std::string& symbol) {
        return engine.find_contract(symbol);
    };
    // Send at market, wait for the fill, and print what was booked for it. False when it did not
    // fill: then it is cancelled, and what follows is not sent.
    const auto fill = [&](const char* what, model::ComboType type,
                          const std::unordered_map<std::string, std::string>& roles,
                          model::Side side) -> bool {
        std::cout << "[" << what << "]\n";
        const auto [legs, _] = model::build_combo(roles, type, side, find_contract);
        const std::string id = engine.event_loop()->on_event_thread([&] {
            return engine.send_order(
                optra_tests::record_of(engine, strategy),
                model::order_request(legs, 0.0, 1, model::OrderType::MARKET, type));
        });
        if (id.empty()) {
            ADD_FAILURE() << what << ": no order went (refused?)";
            return false;
        }
        const auto status = wait_for(
            engine, id, [](Status s) { return !working(s) && s != Status::SUBMITTING; });
        EXPECT_EQ(status, Status::FILLED) << what;
        if (status != Status::FILLED) {
            if (working(status)) {
                engine.event_loop()->on_event_thread(
                    [&] { engine.cancel_order({.order_id = id, .symbol = "SPXW"}); });
                wait_for(engine, id, [](Status s) { return !working(s); });
            }
            return false;
        }
        std::this_thread::sleep_for(3s); // the executions after the order's status
        engine.event_loop()->on_event_thread([&] {
            const model::Order* o = engine.bookkeeping_engine()->find_order(id);
            std::cout << "        order " << id << " " << o->symbol << " combo=" << o->is_combo()
                      << " " << model::to_string(o->combo_type) << " traded " << o->filled << "\n";
            for (const model::Fill& t : engine.bookkeeping_engine()->fills()) {
                if (t.order_id == id) {
                    std::cout << "        fill " << t.fill_id << " " << t.symbol << " "
                              << model::to_string(t.side) << " " << t.quantity << " @ "
                              << t.price << "\n";
                }
            }
        });
        return true;
    };
    using model::ComboType;
    using model::Side;
    const std::unordered_map<std::string, std::string> spread{{"long_leg", low},
                                                             {"short_leg", high}};
    (void)(fill("spread: buy", ComboType::SPREAD, spread, Side::BUY) &&
           fill("spread: sell back", ComboType::SPREAD, spread, Side::SELL) &&
           fill("single leg: buy", ComboType::SINGLE_LEG, {{"option", high}}, Side::BUY) &&
           fill("single leg: sell back", ComboType::SINGLE_LEG, {{"option", high}},
                Side::SELL));

    // Flat again, contract by contract, as the strategy's holding has it.
    const std::string held = engine.event_loop()->on_event_thread([&] {
        std::string out;
        if (const model::StrategyHolding* h = engine.bookkeeping_engine()->find_holding(strategy)) {
            h->for_each_open_option([&](const model::OptionPosition& p) {
                out += std::format("{} {} ", p.symbol, p.quantity);
            });
        }
        return out;
    });
    EXPECT_EQ(held, "") << "the strategy still holds";
    std::cout << "[this run]\n";
    clean_up([&](const std::string& owner) { return owner == strategy; });
    engine.gateway()->disconnect();
    engine.close();
}
} // namespace
