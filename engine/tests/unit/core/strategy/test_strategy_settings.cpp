/**
 * Strategy settings are checked against what the class declares, and instances are named so the
 * same class can run twice on one portfolio.
 */
#include "core/strategy/strategy_engine.hpp"
#include "tests/lone_backtest.hpp"
#include "strategy/strategy_registry.hpp"
#include "strategy/strategy.hpp"
#include <gtest/gtest.h>
#include <stdexcept>

namespace {

using strategy::StrategyRegistry;

class SettingsProbeStrategy final : public strategy::Strategy {
  public:
    struct Setting {
        strategy::Param<int> lookback{"lookback", 60, 2, 390};
        strategy::Param<double> threshold{"threshold", 2.0, 0.0, 10.0};
    };
    using Strategy::Strategy;

    void on_timer() override {}

    int lookback() const { return setting_.lookback; }
    double threshold() const { return setting_.threshold; }

  private:
    const Setting setting_ = read_setting<Setting>();
};

REGISTER_STRATEGY(SettingsProbeStrategy);

TEST(StrategySettings, ASettingDeclaresItsParametersInOrderItsBasesFirst) {
    struct Base {
        strategy::Param<double> budget{"budget", 1500, 1, 1e6};
    };
    struct Derived : Base {
        strategy::Param<int> minutes{"minutes", 30, 1, 120};
    };
    const std::vector<model::Parameter> declared = model::parameters_of<Derived>();
    ASSERT_EQ(declared.size(), 2U);
    EXPECT_EQ(declared[0].name, "budget");
    EXPECT_FALSE(declared[0].integer);
    EXPECT_EQ(declared[1].name, "minutes");
    EXPECT_TRUE(declared[1].integer); // whole numbers: a Param<int>
    EXPECT_EQ(declared[1].default_value, 30);
    EXPECT_EQ(Derived{}.minutes, 30); // outside a declaration, a Setting is its defaults
}

TEST(StrategySettings, DeclaredDefaultsAreOverriddenBySetting) {
    const auto resolved =
        StrategyRegistry::resolve_setting("SettingsProbeStrategy", {{"lookback", 30}});
    EXPECT_EQ(resolved.at("lookback"), 30);
    EXPECT_EQ(resolved.at("threshold"), 2.0);
    EXPECT_EQ(resolved.at("timer_trigger"), 1); // the template's own parameter
}

TEST(StrategySettings, AnythingTheClassDoesNotAllowIsRejected) {
    const std::string cls = "SettingsProbeStrategy";
    EXPECT_THROW(StrategyRegistry::resolve_setting(cls, {{"lookbak", 30}}), std::invalid_argument);
    EXPECT_THROW(StrategyRegistry::resolve_setting(cls, {{"lookback", 1}}), std::invalid_argument);
    EXPECT_THROW(StrategyRegistry::resolve_setting(cls, {{"lookback", 30.5}}),
                 std::invalid_argument);
    EXPECT_THROW(StrategyRegistry::resolve_setting("NoSuchStrategy", {}), std::invalid_argument);
}

class StrategyInstances : public ::testing::Test {
  protected:
    void SetUp() override { main.portfolios()->ensure_portfolio("SPY"); }

    const SettingsProbeStrategy* probe(const std::string& name) {
        return static_cast<const SettingsProbeStrategy*>(se->find_strategy(name));
    }

    test::LoneBacktest backtest;
    core::Main& main = backtest.main();
    core::StrategyEngine* se = main.strategy_engine();
};

TEST_F(StrategyInstances, OneClassRunsTwiceOnAPortfolioUnderTwoNames) {
    EXPECT_EQ(se->add_strategy({.class_name = "SettingsProbeStrategy",
                                .portfolio = "SPY"}), "SettingsProbeStrategy_SPY");
    EXPECT_EQ(se->add_strategy({.name = "fast",
                                .class_name = "SettingsProbeStrategy",
                                .portfolio = "SPY",
                                .setting = {{"lookback", 30}}}), "fast");

    ASSERT_NE(probe("SettingsProbeStrategy_SPY"), nullptr);
    ASSERT_NE(probe("fast"), nullptr);
    EXPECT_EQ(probe("SettingsProbeStrategy_SPY")->lookback(), 60);
    EXPECT_EQ(probe("fast")->lookback(), 30);
    EXPECT_EQ(probe("fast")->portfolio_name(), "SPY");
    EXPECT_EQ(se->find_record("fast")->class_name, "SettingsProbeStrategy");
}

TEST_F(StrategyInstances, ANameInUseIsRejectedNotReplaced) {
    se->add_strategy({.name = "s1", .class_name = "SettingsProbeStrategy", .portfolio = "SPY"});
    const auto* first = se->find_strategy("s1");
    EXPECT_THROW(se->add_strategy({.name = "s1",
                                   .class_name = "SettingsProbeStrategy",
                                   .portfolio = "SPY",
                                   .setting = {{"lookback", 30}}}),
                 std::runtime_error);
    EXPECT_EQ(se->find_strategy("s1"), first);
}

TEST_F(StrategyInstances, ABadSettingAddsNothing) {
    EXPECT_THROW(se->add_strategy({.class_name = "SettingsProbeStrategy",
                                   .portfolio = "SPY",
                                   .setting = {{"threshold", 11}}}),
                 std::invalid_argument);
    EXPECT_EQ(se->find_strategy("SettingsProbeStrategy_SPY"), nullptr);
}

} // namespace
