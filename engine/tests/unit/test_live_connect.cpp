/**
 * Connecting the live engine to a broker (a stand-in at the venue port): it catches up, then
 * checks the account's positions against the holdings, and a strategy starts only once they
 * agree. Not connected, a disagreement or a catch-up the broker did not answer keep it from
 * starting; set right at the broker, ReconcilePositions lets it start.
 */
#include "fake_broker.hpp"
#include "core/engine_live.hpp"
#include "strategy/strategy_registry.hpp"
#include "strategy/template.hpp"
#include <cstdlib>
#include <filesystem>
#include <gtest/gtest.h>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

using optra_tests::FakeBroker;

/** Does nothing: what is tested is whether it may start. */
class IdleStrategy final : public strategy::OptionStrategyTemplate {
  public:
    using OptionStrategyTemplate::OptionStrategyTemplate;

    void on_init_logic() override {}
    void on_stop_logic() override {}
    void on_timer_logic() override {}
};

REGISTER_STRATEGY(IdleStrategy);

class LiveConnect : public ::testing::Test {
  protected:
    void SetUp() override {
        path_ = ::testing::TempDir() + "optra_" +
                ::testing::UnitTest::GetInstance()->current_test_info()->name() + ".db";
        remove_files();
        setenv("OPTRA_DB", path_.c_str(), 1);
        unsetenv("OPTRA_RECONCILE");
        auto broker = std::make_unique<FakeBroker>();
        broker_ = broker.get();
        engine_ = std::make_unique<core::live::MainEngine>(std::move(broker));
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

    std::string path_;
    FakeBroker* broker_ = nullptr;
    std::unique_ptr<core::live::MainEngine> engine_;
    std::string name_;
};

TEST_F(LiveConnect, NotConnectedAStrategyDoesNotStart) {
    EXPECT_EQ(engine_->positions_check().first, "not yet");
    EXPECT_NE(start().find("Connect first"), std::string::npos);
}

TEST_F(LiveConnect, ConnectedWithPositionsThatAgreeItStarts) {
    engine_->connect();
    EXPECT_EQ(broker_->catch_ups, 1);
    EXPECT_EQ(engine_->positions_check().first, "agree");
    EXPECT_EQ(start(), "");
}

TEST_F(LiveConnect, PositionsThatDisagreeKeepItFromStartingUntilSetRight) {
    broker_->account.positions.push_back({.symbol = "SPY", .quantity = 100}); // not held here
    engine_->connect();
    EXPECT_EQ(engine_->positions_check().first, "disagree");
    EXPECT_NE(start().find("disagree"), std::string::npos);

    broker_->account.positions.clear(); // set right at the broker
    engine_->reconcile_positions();
    EXPECT_EQ(engine_->positions_check().first, "agree");
    EXPECT_EQ(start(), "");
}

TEST_F(LiveConnect, ACatchUpIbDidNotAnswerLeavesThePositionsNotChecked) {
    broker_->catch_up_fails = true;
    engine_->connect();
    EXPECT_EQ(engine_->positions_check().first, "failed");
    EXPECT_NE(start().find("not checked"), std::string::npos);
}

} // namespace
