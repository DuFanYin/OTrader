/**
 * Connecting the live engine to a broker (a stand-in at the venue port): it catches up, then
 * checks the account's positions against the holdings, and a strategy starts only once they
 * agree. Not connected, a disagreement or a catch-up the broker did not answer keep it from
 * starting; set right at the broker, ReconcilePositions lets it start. A day's contracts are
 * synced, saved and rolled in together, or, the sync failing, not at all. The trading days connect
 * on their own once a gateway that was not up comes up, and begin the first day once its contracts
 * come in; a stop does not wait for a sync the broker does not answer.
 */
#include "tests/unit/core/live/fake_broker.hpp"
#include "assembly/live.hpp"
#include "model/symbol.hpp"
#include "model/utility.hpp"
#include "strategy/strategy_registry.hpp"
#include "strategy/strategy.hpp"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <gtest/gtest.h>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using optra_tests::FakeBroker;

/** Does nothing: what is tested is whether it may start. */
class IdleStrategy final : public strategy::Strategy {
  public:
    using Strategy::Strategy;

    void on_init() override {}
    void on_stop() override {}
    void on_timer() override {}
};

REGISTER_STRATEGY(IdleStrategy);

class LiveConnect : public ::testing::Test {
  protected:
    void SetUp() override {
        path_ = ::testing::TempDir() + "optra_" +
                ::testing::UnitTest::GetInstance()->current_test_info()->name() + ".db";
        remove_files();
        setenv("OPTRA_DB", path_.c_str(), 1);
        unsetenv("OPTRA_BROKER_CHECK");
        auto broker = std::make_unique<FakeBroker>();
        broker_ = broker.get();
        engine_ = live::make_main(std::move(broker));
        name_ = engine_->add_strategy("IdleStrategy", "SPY", {});
        engine_->init_strategy(name_);
    }
    void TearDown() override {
        engine_->close();
        engine_.reset();
        unsetenv("OPTRA_DB");
        remove_files();
    }
    void remove_files() const {
        for (const char* suffix : {"", "-wal", "-shm"}) {
            std::filesystem::remove(path_ + suffix);
        }
    }
    /** start_strategy's refusal, or "" when it started. */
    auto start() -> std::string {
        try {
            engine_->start_strategy(name_);
            return "";
        } catch (const std::runtime_error& e) {
            return e.what();
        }
    }

    auto listed(const std::string& option) -> bool {
        return engine_->event_loop()->on_event_thread(
            [&] { return engine_->find_portfolio("SPY")->option(option).has_value(); });
    }

    auto positions_check() -> std::pair<std::string, std::string> {
        return engine_->event_loop()->on_event_thread(
            [this] { return engine_->bookkeeping_engine()->broker_check(); });
    }

    std::string path_;
    FakeBroker* broker_ = nullptr;
    std::unique_ptr<core::Main> engine_;
    std::string name_;
};

TEST_F(LiveConnect, NotConnectedAStrategyDoesNotStart) {
    EXPECT_EQ(positions_check().first, "not yet");
    EXPECT_NE(start().find("Connect first"), std::string::npos);
}

TEST_F(LiveConnect, ConnectedWithPositionsThatAgreeItStarts) {
    engine_->connect();
    EXPECT_EQ(broker_->catch_ups, 1);
    EXPECT_EQ(positions_check().first, "agree");
    EXPECT_EQ(start(), "");
}

TEST_F(LiveConnect, PositionsThatDisagreeKeepItFromStartingUntilSetRight) {
    broker_->account.positions.push_back({.symbol = "SPY", .quantity = 100}); // not held here
    engine_->connect();
    EXPECT_EQ(positions_check().first, "disagree");
    EXPECT_NE(start().find("disagree"), std::string::npos);

    broker_->account.positions.clear(); // set right at the broker
    engine_->check_with_broker();
    EXPECT_EQ(positions_check().first, "agree");
    EXPECT_EQ(start(), "");
}

TEST_F(LiveConnect, ACatchUpIbDidNotAnswerLeavesThePositionsNotChecked) {
    broker_->catch_up_fails = true;
    engine_->connect();
    EXPECT_EQ(positions_check().first, "failed");
    EXPECT_NE(start().find("not checked"), std::string::npos);
}

const std::chrono::year_month_day kExpiry{std::chrono::year(2099), std::chrono::December,
                                         std::chrono::day(18)};
const std::string kPut = model::option_symbol(
    {.root = "SPY", .expiry = kExpiry, .type = model::OptionType::PUT, .strike = 555});

/** SPY and one of its puts, as a broker's sync gives them. */
auto spy_and_a_put() -> std::vector<model::Contract> {
    model::Contract spy;
    spy.symbol = "SPY";
    spy.product = model::Product::EQUITY;
    spy.trading_class = "SPY";
    model::Contract option;
    option.symbol = kPut;
    option.product = model::Product::OPTION;
    option.multiplier = 100;
    option.trading_class = "SPY";
    option.option_strike = 555;
    option.option_type = model::OptionType::PUT;
    option.option_expiry = model::option_expiry_time(kExpiry);
    option.option_underlying = "SPY";
    return {spy, option};
}

TEST_F(LiveConnect, TheDaysContractsAreSyncedSavedAndRolledInOrNotAtAll) {
    broker_->contracts = spy_and_a_put();
    const std::vector<core::Main::Synced> day{{"SPY", 1}};
    EXPECT_EQ(engine_->roll_day(day).size(), 2U);
    EXPECT_TRUE(listed(kPut));
    const std::vector<model::Contract> saved = engine_->database()->load_option_contracts();
    EXPECT_TRUE(std::ranges::any_of(saved, [&](const auto& c) { return c.symbol == kPut; }));

    broker_->sync_fails = true;
    broker_->contracts.clear();
    EXPECT_THROW(engine_->roll_day(day), std::runtime_error);
    EXPECT_TRUE(listed(kPut)); // the day before's stay
}

TEST_F(LiveConnect, TheTradingDaysConnectOnceTheGatewayComesUp) {
    using namespace std::chrono;
    broker_->refuse_connect = true; // started before its gateway
    live::TradingDays days(*engine_, 20ms);
    days.start({}, false);
    EXPECT_FALSE(broker_->connected);
    std::this_thread::sleep_for(100ms); // a few tries
    EXPECT_FALSE(broker_->connected);
    broker_->refuse_connect = false;
    for (int i = 0; i < 200 && !broker_->connected; ++i) {
        std::this_thread::sleep_for(10ms);
    }
    EXPECT_TRUE(broker_->connected);
    EXPECT_EQ(positions_check().first, "agree"); // caught up and checked, as a first connect is
    days.stop();
}

TEST_F(LiveConnect, AStopDoesNotWaitForASyncTheBrokerDoesNotAnswer) {
    using namespace std::chrono;
    broker_->contracts = spy_and_a_put();
    broker_->sync_hangs = true; // IB stopped answering: the sync would wait its five minutes
    live::TradingDays days(*engine_, 20ms);
    std::thread beginning([&] { days.start({{"SPY", 1}}, false); }); // the day begun first
    std::this_thread::sleep_for(100ms);
    const auto asked = steady_clock::now();
    days.interrupt(); // as the stop signal does
    beginning.join();
    days.stop();
    EXPECT_LT(steady_clock::now() - asked, 2s);
    EXPECT_FALSE(listed(kPut)); // nothing of the day rolled in
    EXPECT_FALSE(broker_->connected);
}

TEST_F(LiveConnect, TheFirstDayWaitsForItsContractsThenBegins) {
    using namespace std::chrono;
    broker_->contracts = spy_and_a_put();
    broker_->sync_fails = true; // IB not answering the sync yet
    live::TradingDays days(*engine_, 20ms);
    days.start({{"SPY", 1}}, true);
    EXPECT_TRUE(broker_->connected);
    std::this_thread::sleep_for(100ms); // a few tries
    EXPECT_FALSE(listed(kPut));
    broker_->sync_fails = false;
    bool rolled = false;
    for (int i = 0; i < 200 && !rolled; ++i) {
        std::this_thread::sleep_for(10ms);
        rolled = listed(kPut);
    }
    EXPECT_TRUE(rolled);
    days.stop();
    EXPECT_FALSE(broker_->connected); // stopped: disconnected
}

} // namespace
