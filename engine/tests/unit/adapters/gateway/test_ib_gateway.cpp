/**
 * IbGateway's checks before IB is asked: it trades one named paper account (a live one only once
 * root names it), and checks a contract sync against the platform's portfolios first. Pointed at
 * a port nothing listens on: an account it may trade gets as far as reaching for IB. Then against a
 * TWS of the test's (tests/fake_tws.hpp), what IB rarely does on a paper account: an error in the
 * middle of a sync, another client's order, a sync that stops getting answers; an order sent from
 * its caller that IB answers at once.
 */
#include "adapters/gateway/ib_gateway.hpp"
#include "adapters/ib_api/ib_api.hpp"
#include "model/order_builder.hpp"
#include "model/portfolio_spec.hpp"
#include "tests/fake_tws.hpp"
#include "utilities/logger.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <future>
#include <fstream>
#include <gtest/gtest.h>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

/** Nothing listens there: connecting fails at once. */
constexpr int kClosedPort = 1;

struct Gateway {
    Gateway() {
        adapters::IbSettings& s = gateway.default_setting();
        s.host = "127.0.0.1";
        s.port = kClosedPort;
        s.account = "DU1"; // a paper account
    }
    /** connect()'s refusal ("" when it connected). */
    auto connect_error() -> std::string {
        try {
            gateway.connect();
            return "";
        } catch (const std::runtime_error& e) {
            return e.what();
        }
    }
    adapters::IbGateway gateway{nullptr};
};

TEST(IbGateway, TradesOneNamedPaperAccount) {
    Gateway g;
    g.gateway.default_setting().account = "";
    EXPECT_NE(g.connect_error().find("Set IB_ACCOUNT"), std::string::npos); // none named
    g.gateway.default_setting().account = "U1234567";
    EXPECT_NE(g.connect_error().find("not a paper account"), std::string::npos); // a live one
    // Named in a file a user wrote (not root's): still refused.
    const std::string file = ::testing::TempDir() + "optra_live_account";
    std::ofstream(file) << "U1234567\n";
    g.gateway.default_setting().live_account_file = file;
    EXPECT_NE(g.connect_error().find("not a paper account"), std::string::npos);
    std::filesystem::remove(file);

    g.gateway.default_setting().account = "DU2"; // a paper one: on to IB, which is not there
    EXPECT_NE(g.connect_error().find("Cannot reach IB"), std::string::npos);
    EXPECT_FALSE(g.gateway.is_connected());
}

TEST(IbGateway, ALiveAccountsStrategiesAreOnlyThoseRootApproved) {
    adapters::IbSettings s;
    s.account = "DU1";
    EXPECT_FALSE(s.is_live_account());
    s.account = "U1234567";
    EXPECT_TRUE(s.is_live_account());
    // Listed in a file a user wrote (not root's): none.
    s.live_strategies_file = ::testing::TempDir() + "optra_live_strategies";
    std::ofstream(s.live_strategies_file) << "ComboStrategy e20212171aaeb093 entry_minute=30\n";
    EXPECT_TRUE(s.live_strategies().empty());
    std::filesystem::remove(s.live_strategies_file);
    EXPECT_TRUE(s.live_strategies().empty()); // nor with no file
}

TEST(IbGateway, ASyncIsCheckedBeforeIbIsAsked) {
    Gateway g;
    EXPECT_THROW((void)g.gateway.sync_contracts("DIA", 7), std::invalid_argument); // no portfolio
    EXPECT_THROW((void)g.gateway.sync_contracts("SPXW", -1), std::invalid_argument);
    EXPECT_THROW((void)g.gateway.sync_contracts("SPXW", 7), std::runtime_error); // not connected
}

TEST(IbGateway, EveryPortfolioIsListedAtIb) {
    for (const model::PortfolioSpec& p : model::kPortfolios) {
        EXPECT_NE(adapters::find_listing(p.underlying), nullptr) << p.name;
        EXPECT_NE(adapters::find_listing(p.hedge), nullptr) << p.name; // what it hedges with
    }
}

/** What the gateway logged and the orders it passed on, waited for by a test. */
struct Seen final : utilities::Logger {
    void log(const utilities::LogRecord& record) override {
        std::scoped_lock lock(mutex);
        lines.push_back(record.msg);
        changed.notify_all();
    }
    void order(const model::Order& order) {
        std::scoped_lock lock(mutex);
        orders.push_back(order);
        changed.notify_all();
    }
    auto wait(const std::function<bool()>& done, std::chrono::seconds within) -> bool {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, within, done);
    }
    auto logged(const std::string& part) -> bool {
        return std::ranges::any_of(lines, [&](const std::string& l) {
            return l.find(part) != std::string::npos;
        });
    }
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<std::string> lines;
    std::vector<model::Order> orders;
};

/** A gateway connected to a TWS of the test's. */
struct Connected {
    Connected() {
        gateway.set_callbacks([this](const model::Order& o) { seen.order(o); },
                              [](const model::Fill&) {});
        adapters::IbSettings& s = gateway.default_setting();
        s.host = "127.0.0.1";
        s.port = tws.port();
        s.account = "DU1";
        gateway.connect();
    }
    /** The sync's request for its underlying's contract (SPX for SPXW), answered. */
    void answer_underlying() {
        const auto asked = tws.request(tests::FakeTws::kReqContractData);
        ASSERT_TRUE(asked && asked->size() > 2);
        const std::string& id = (*asked)[2]; // after the message id and its version
        tws.send({"10", id, "SPX", "IND", "", "0", "", "CBOE", "USD", "SPX", "SPX", "SPX",
                  "416904", "0.01", ""},
                 60);
        tws.send({"52", "1", id});
    }
    tests::FakeTws tws;
    Seen seen;
    adapters::IbGateway gateway{&seen};
};

TEST(IbGateway, AnErrorInTheMiddleOfASyncEndsItWithIbsReason) {
    Connected c;
    std::future<std::vector<model::Contract>> sync = c.gateway.sync_contracts("SPXW", 0);
    c.answer_underlying();
    const auto expiries = c.tws.request(tests::FakeTws::kReqSecDefOptParams);
    ASSERT_TRUE(expiries && expiries->size() > 1); // asked once the underlying is known
    c.tws.send({"4", "2", "999", "2104", "Market data farm connection is OK", ""}); // not the sync's
    c.tws.send({"4", "2", (*expiries)[1], "200", "No security definition has been found", ""});
    ASSERT_EQ(sync.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    try {
        (void)sync.get();
        FAIL() << "the sync succeeded";
    } catch (const std::exception& e) {
        EXPECT_NE(std::string(e.what()).find("IB 200: No security definition"), std::string::npos)
            << e.what();
    }
    std::future<std::vector<model::Contract>> again = c.gateway.sync_contracts("SPXW", 0);
    EXPECT_TRUE(again.valid()); // the failed one is gone: another may start
}

TEST(IbGateway, AnotherClientsOrderIsPassedOnAndFollowed) {
    Connected c;
    // IB tells every client of an order another one placed (here client 7): openOrder, then its
    // status. The engine is told of it as one leg, to show and cancel it.
    c.tws.send({"5", "77", "756733", "SPY", "STK", "", "0", "", "", "SMART", "USD", "SPY", "SPY",
                "BUY", "3", "LMT", "1.5", ""},
               400);
    ASSERT_TRUE(c.seen.wait([&] { return !c.seen.orders.empty(); }, std::chrono::seconds(5)));
    {
        std::scoped_lock lock(c.seen.mutex);
        const model::Order& o = c.seen.orders.front();
        EXPECT_EQ(o.order_id, "77");
        EXPECT_EQ(o.symbol, "SPY");
        EXPECT_EQ(o.quantity, 3.0);
        EXPECT_EQ(o.price, 1.5);
        EXPECT_EQ(o.status, model::Status::SUBMITTING);
        ASSERT_EQ(o.legs.size(), 1U);
        EXPECT_EQ(o.legs.front().side, model::Side::BUY);
    }
    c.tws.send({"3", "88", "Submitted", "0", "1", "0", "0", "0", "0", "7", "", "0"}); // unknown
    c.tws.send({"3", "77", "Cancelled", "0", "3", "0", "0", "0", "0", "7", "", "0"});
    ASSERT_TRUE(c.seen.wait([&] { return c.seen.orders.size() >= 2; }, std::chrono::seconds(5)));
    std::scoped_lock lock(c.seen.mutex);
    ASSERT_EQ(c.seen.orders.size(), 2U); // nothing for the order no client told it of
    EXPECT_EQ(c.seen.orders.back().order_id, "77");
    EXPECT_EQ(c.seen.orders.back().status, model::Status::CANCELLED);
}

TEST(IbGateway, AnOrderGoesOutFromItsCallerAndIbsAnswerAtOnceFindsIt) {
    Connected c;
    const std::array leg{model::Leg{.ratio = 1, .side = model::Side::BUY, .symbol = "SPY"}};
    const std::string id =
        c.gateway.send_order(model::order_request(leg, 500.0, 2, model::OrderType::LIMIT));
    ASSERT_FALSE(id.empty());
    const auto placed = c.tws.request(tests::FakeTws::kPlaceOrder);
    ASSERT_TRUE(placed && placed->size() > 1);
    // Answered as soon as it is read: the message thread has the order in its book by then.
    c.tws.send({"3", id, "Filled", "2", "0", "500", "0", "0", "500", "1", "", "0"});
    ASSERT_TRUE(c.seen.wait(
        [&] {
            return std::ranges::any_of(c.seen.orders, [&](const model::Order& o) {
                return o.order_id == id && o.status == model::Status::FILLED;
            });
        },
        std::chrono::seconds(5)));
    std::scoped_lock lock(c.seen.mutex);
    EXPECT_EQ(c.seen.orders.front().status, model::Status::SUBMITTING); // as it went
    EXPECT_EQ(c.seen.orders.back().filled, 2.0);
}

TEST(IbGateway, ASyncThatStopsGettingAnswersSaysWhatItWaitsFor) {
    Connected c;
    std::future<std::vector<model::Contract>> sync = c.gateway.sync_contracts("SPXW", 0);
    c.answer_underlying();
    ASSERT_TRUE(c.tws.request(tests::FakeTws::kReqSecDefOptParams)); // asked, never answered
    // Every ten seconds while nothing comes.
    EXPECT_TRUE(c.seen.wait([&] { return c.seen.logged("Syncing SPXW: waiting for the option expiries"); },
                            std::chrono::seconds(15)));
    EXPECT_EQ(sync.wait_for(std::chrono::seconds(0)), std::future_status::timeout); // still running
}

} // namespace
