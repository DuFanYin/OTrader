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
 * Gateway. The tests connect as IB client ids 41 to 48: no other session may use them.
 *
 * Nothing fills: the one order is BUY 1 SPY at 100, and the strategies run without prices (market
 * data type 1, which a paper account does not get); but ComboFillsAreBookedAsLegs, which trades at
 * market and runs only with OPTRA_IB_PAPER_FILLS=1.
 */

#include "core/engines/execution.hpp"
#include "core/engines/option_strategy.hpp"
#include "core/engine_live.hpp"
#include "model/combo_builder.hpp"
#include "strategy/template.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sqlite3.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <format>
#include <functional>
#include <gtest/gtest.h>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using model::Status;

constexpr double kPrice = 100.0; // far below SPY: the order works and never fills
constexpr auto kWait = 30s;
constexpr auto kRestartWait = 180s; // IB Gateway closes about a minute after IBC is asked
constexpr auto kSteady = 15s;       // connected this long: the reconnect's catch-up has run

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
 * still in the WAL); with no_records, its orders and trades left out: IB reports this client
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
        rc = sqlite3_exec(dst, "DELETE FROM orders; DELETE FROM trades;", nullptr, nullptr,
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

auto started(core::live::MainEngine& engine, const std::string& name) -> bool {
    return engine.on_event_thread([&] {
        const strategy::OptionStrategyTemplate* s =
            engine.option_strategy_engine()->get_strategy(name);
        return s != nullptr && s->started();
    });
}

/** The order's status as the engine has it. */
auto status_of(core::live::MainEngine& engine, const std::string& orderid) -> std::optional<Status> {
    return engine.on_event_thread([&]() -> std::optional<Status> {
        const model::OrderData* o = engine.execution_engine()->get_order(orderid);
        return o != nullptr ? std::optional(o->status) : std::nullopt;
    });
}

/** Wait (up to kWait) until the order's status satisfies done; prints each change. */
auto wait_for(core::live::MainEngine& engine, const std::string& orderid,
              const std::function<bool(Status)>& done) -> std::optional<Status> {
    std::optional<Status> last;
    const auto until = std::chrono::steady_clock::now() + kWait;
    while (std::chrono::steady_clock::now() < until) {
        const std::optional<Status> now = status_of(engine, orderid);
        if (now != last && now) {
            std::cout << "        order " << orderid << ": " << model::to_string(*now) << "\n";
        }
        last = now;
        if (now && done(*now)) {
            return now;
        }
        std::this_thread::sleep_for(200ms);
    }
    return last;
}

auto working(Status s) -> bool { return s == Status::NOTTRADED || s == Status::PARTTRADED; }

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
auto wait_until(core::live::MainEngine& engine, bool up) -> bool {
    const auto until = std::chrono::steady_clock::now() + kRestartWait;
    auto up_since = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() < until) {
        const bool connected = engine.connected();
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
        core::live::MainEngine engine;
        EXPECT_NO_THROW(engine.connect()) << "IB_ACCOUNT";
        engine.disconnect();
        engine.close();
    }
    for (const auto& [what, account] : refused) {
        if (account) {
            setenv("IB_ACCOUNT", account->c_str(), 1);
        } else {
            unsetenv("IB_ACCOUNT");
        }
        core::live::MainEngine engine;
        EXPECT_THROW(engine.connect(), std::exception) << what;
        engine.disconnect();
        engine.close();
    }
}

TEST_F(IbPaper, AResumedEngineStartsTheStrategiesThatWereRunning) {
    const std::string db = build_file("ib_paper_resume.db");
    copy_db(db);
    use(db, 43);
    std::string name;
    {
        core::live::MainEngine engine;
        name = engine.add_strategy("IvMeanRevertStrategy", "SPY", {});
        engine.connect();
        EXPECT_EQ(engine.positions_check().first, "agree") << engine.positions_check().second;
        engine.start_market_data_update();
        engine.init_strategy(name);
        engine.start_strategy(name);
        ASSERT_TRUE(started(engine, name));
        engine.disconnect();
        engine.close();
    }
    core::live::MainEngine engine;
    EXPECT_FALSE(started(engine, name)) << "back stopped, until resume()";
    engine.resume();
    EXPECT_EQ(engine.positions_check().first, "agree") << engine.positions_check().second;
    EXPECT_TRUE(engine.market_data_running());
    EXPECT_TRUE(started(engine, name));
    const int sent = engine.on_event_thread([&] {
        int n = 0;
        for (const model::OrderData& o : engine.execution_engine()->get_all_orders()) {
            n += engine.execution_engine()->get_strategy_name_for_order(o.orderid) == name ? 1 : 0;
        }
        return n;
    });
    EXPECT_EQ(sent, 0) << "it sent nothing: no prices";
    engine.stop_strategy(name);
    engine.disconnect();
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
    std::string orderid;

    std::cout << "[1] Send a limit order far from the market\n";
    {
        core::live::MainEngine engine;
        engine.add_strategy("StraddleTestStrategy", "SPY", {}, strategy);
        engine.connect();
        orderid = engine.on_event_thread([&] {
            const auto ids = engine.option_strategy_engine()->send_order(
                strategy, "SPY", model::Direction::LONG, kPrice, 1,
                model::OrderType::LIMIT);
            return ids.empty() ? std::string{} : ids.front();
        });
        ASSERT_FALSE(orderid.empty()) << "the gateway took no order";
        EXPECT_TRUE(working(wait_for(engine, orderid, [](Status s) { return working(s); })))
            << "IB has it working";
        std::cout << "[2] Stop the engine; the order stays at IB\n";
        engine.disconnect();
        engine.close();
    }

    std::cout << "[3] A new engine on the same database\n";
    core::live::MainEngine engine;
    EXPECT_TRUE(engine.on_event_thread([&] {
        return engine.option_strategy_engine()->get_strategy(strategy) != nullptr;
    })) << "the strategy is back";
    EXPECT_TRUE(working(status_of(engine, orderid))) << "its order is back, working";
    engine.connect();
    std::this_thread::sleep_for(3s); // the catch-up's updates, if any, have come in
    EXPECT_TRUE(working(status_of(engine, orderid))) << "still working after catching up";

    if (const std::string port = env("IBC_COMMAND_PORT"); !port.empty()) {
        std::cout << "[4] IBC restarts IB Gateway; the engine reconnects and catches up\n";
        EXPECT_TRUE(ask_ibc_to_restart(std::stoi(port))) << "IBC took the restart";
        EXPECT_TRUE(wait_until(engine, false)) << "the connection dropped";
        EXPECT_TRUE(wait_until(engine, true)) << "the connection is back";
        EXPECT_TRUE(working(status_of(engine, orderid))) << "still working after the reconnect";
    } else {
        std::cout << "[4] IB Gateway's restart left out: no IBC_COMMAND_PORT\n";
    }

    std::cout << "[5] Cancel it, and whatever earlier runs left working\n";
    const std::vector<std::string> leftover = engine.on_event_thread([&] {
        std::vector<std::string> ids;
        for (const model::OrderData& o : engine.execution_engine()->get_all_active_orders()) {
            const std::string owner =
                engine.execution_engine()->get_strategy_name_for_order(o.orderid);
            if (o.orderid != orderid && owner.starts_with("PaperOrderTest_")) {
                ids.push_back(o.orderid);
            }
        }
        return ids;
    });
    engine.on_event_thread([&] {
        for (const std::string& id : leftover) {
            engine.cancel_order({.orderid = id, .symbol = "SPY"});
        }
        engine.cancel_order({.orderid = orderid, .symbol = "SPY"});
    });
    EXPECT_EQ(wait_for(engine, orderid, [](Status s) { return !working(s); }), Status::CANCELLED)
        << "IB cancelled it";
    for (const std::string& id : leftover) {
        EXPECT_EQ(wait_for(engine, id, [](Status s) { return !working(s); }), Status::CANCELLED)
            << "IB cancelled an earlier run's order " << id;
    }
    engine.disconnect();
    engine.close();
}

/** Combo fills at IB are booked as leg fills (the BAG execution is not booked), and the holding is
 * flat after selling back. An SPXW call spread is bought and sold back, then one call is bought
 * and sold as a single-leg combo, all at market. It trades, so it runs only with
 * OPTRA_IB_PAPER_FILLS=1 during market hours. It uses its own database copy, with risk limits
 * that allow a spread (the paper account's limits refuse a short leg). */
TEST_F(IbPaper, ComboFillsAreBookedAsLegs) {
    if (env("OPTRA_IB_PAPER_FILLS") != "1") {
        GTEST_SKIP() << "it trades: set OPTRA_IB_PAPER_FILLS=1 during market hours";
    }
    const std::string db = build_file("ib_paper_fills.db");
    copy_db(db, /*no_records=*/true);
    use(db, 47);
    setenv("IB_MARKET_DATA_TYPE", "4", 1); // delayed quotes: enough to find the money
    const std::string strategy =
        std::format("PaperFillTest_{:%Y%m%d%H%M%S}",
                    std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));

    core::live::MainEngine engine;
    if (env("OPTRA_IB_PAPER_RAW") == "1") {
        engine.set_log_level(utilities::DEBUG); // IB's callbacks as they come (IB orderStatus, ...)
    }
    engine.add_strategy("StraddleTestStrategy", "SPXW", {}, strategy); // never started
    engine.set_risk_settings(
        {{"short_options", 1}, {"max_order_premium", 0}, {"market_max_spread", 0}});
    engine.connect();

    // The first expiry after today, and its strikes nearest the money.
    const std::string today = std::format("SPXW_{:%Y%m%d}", std::chrono::system_clock::now());
    const std::string chain = engine.on_event_thread([&] {
        std::string best;
        for (const auto& [name, _] : engine.get_portfolio("SPXW")->chains) {
            if (name > today && (best.empty() || name < best)) {
                best = name;
            }
        }
        return best;
    });
    ASSERT_FALSE(chain.empty()) << "no SPXW expiry after today in the database";
    engine.on_event_thread([&] { engine.subscribe_chains(strategy, std::vector{chain}); });
    engine.start_market_data_update();
    double forward = 0;
    for (auto until = std::chrono::steady_clock::now() + 90s;
         forward <= 0 && std::chrono::steady_clock::now() < until;
         std::this_thread::sleep_for(500ms)) {
        forward = engine.on_event_thread([&] {
            return engine.get_portfolio("SPXW")->chains.at(chain)->forward;
        });
    }
    ASSERT_GT(forward, 0) << "no quotes for " << chain;
    const auto [low, high] = engine.on_event_thread([&] {
        const model::ChainData* c = engine.get_portfolio("SPXW")->chains.at(chain).get();
        auto it = c->calls.lower_bound(forward);
        const model::OptionData* hi = it != c->calls.end() ? it->second : nullptr;
        const model::OptionData* lo = it != c->calls.begin() ? std::prev(it)->second : nullptr;
        return std::pair{lo, hi};
    });
    ASSERT_TRUE(low != nullptr && high != nullptr);
    std::cout << "        " << chain << " forward " << forward << ": " << low->symbol << " / "
              << high->symbol << "\n";

    const auto options = [&] {
        model::combo::ComboBuildOptions o;
        o.get_contract = [&](const std::string& symbol) {
            return engine.option_strategy_engine()->get_contract(symbol);
        };
        return o;
    }();
    // Send at market, wait for the fill, and print what was booked for it.
    const auto fill = [&](const char* what, model::ComboType type,
                          const std::unordered_map<std::string, const model::OptionData*>& roles,
                          model::Direction direction) {
        std::cout << "[" << what << "]\n";
        const auto [legs, sig] = model::combo::build_combo(roles, type, direction, options);
        const std::string id = engine.on_event_thread([&] {
            const auto ids = engine.option_strategy_engine()->send_combo_order(
                strategy, type, sig, model::Direction::LONG, 0.0, 1, legs,
                model::OrderType::MARKET);
            return ids.empty() ? std::string{} : ids.front();
        });
        if (id.empty()) {
            ADD_FAILURE() << what << ": no order went (refused?)";
            return;
        }
        const auto status = wait_for(
            engine, id, [](Status s) { return !working(s) && s != Status::SUBMITTING; });
        EXPECT_EQ(status, Status::ALLTRADED) << what;
        std::this_thread::sleep_for(3s); // the executions after the order's status
        engine.on_event_thread([&] {
            const model::OrderData* o = engine.execution_engine()->get_order(id);
            std::cout << "        order " << id << " " << o->symbol << " combo=" << o->is_combo
                      << " " << model::to_string(o->combo_type) << " traded " << o->traded << "\n";
            for (const model::TradeData& t : engine.execution_engine()->get_all_trades()) {
                if (t.orderid == id) {
                    std::cout << "        trade " << t.tradeid << " " << t.symbol << " "
                              << model::to_string(t.direction) << " " << t.volume << " @ "
                              << t.price << "\n";
                }
            }
        });
    };
    using model::ComboType;
    using model::Direction;
    const std::unordered_map<std::string, const model::OptionData*> spread{{"long_leg", low},
                                                                          {"short_leg", high}};
    fill("spread: buy", ComboType::SPREAD, spread, Direction::LONG);
    fill("spread: sell back", ComboType::SPREAD, spread, Direction::SHORT);
    fill("single leg: buy", ComboType::SINGLE_LEG, {{"option", high}}, Direction::LONG);
    fill("single leg: sell back", ComboType::SINGLE_LEG, {{"option", high}}, Direction::SHORT);

    // Flat again, contract by contract, as the strategy's holding has it.
    const std::string held = engine.on_event_thread([&] {
        std::string out;
        if (const model::StrategyHolding* h = engine.position_engine()->find_holding(strategy)) {
            h->for_each_open_option([&](const model::OptionPositionData& p) {
                out += std::format("{} {} ", p.symbol, p.quantity);
            });
        }
        return out;
    });
    EXPECT_EQ(held, "") << "the strategy still holds";
    engine.disconnect();
    engine.close();
}
} // namespace
