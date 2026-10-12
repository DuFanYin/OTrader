/**
 * Strategy settings are checked against what the class declares, and instances are named so the
 * same class can run twice on one portfolio.
 */
#include "core/engines/option_strategy.hpp"
#include "core/engine_backtest.hpp"
#include "strategy/strategy_registry.hpp"
#include "strategy/template.hpp"
#include <array>
#include <gtest/gtest.h>
#include <span>
#include <stdexcept>

namespace {

using strategy::StrategyParam;
using strategy::StrategyRegistry;

class SettingsProbeStrategy final : public strategy::OptionStrategyTemplate {
  public:
    SettingsProbeStrategy(strategy::StrategyContext* context, const std::string& name,
                          const std::string& portfolio,
                          const std::unordered_map<std::string, double>& setting)
        : OptionStrategyTemplate(context, name, portfolio, setting),
          lookback_(param<int>("lookback")), threshold_(param("threshold")) {}

    static std::span<const StrategyParam> parameters() { return kParameters; }

    void on_init_logic() override {}
    void on_stop_logic() override {}
    void on_timer_logic() override {}

    int lookback() const { return lookback_; }
    double threshold() const { return threshold_; }

  private:
    // name, default, min, max, integer
    static constexpr std::array kParameters{
        StrategyParam{"lookback", 60, 2, 390, true},
        StrategyParam{"threshold", 2.0, 0.0, 10.0},
    };

    int lookback_;
    double threshold_;
};

REGISTER_STRATEGY(SettingsProbeStrategy);

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
    void SetUp() override { main.portfolio_structure()->ensure_portfolio("SPY"); }

    const SettingsProbeStrategy* probe(const std::string& name) {
        return static_cast<const SettingsProbeStrategy*>(se->get_strategy(name));
    }

    core::backtest::MainEngine main;
    core::OptionStrategyEngine* se = main.option_strategy_engine();
};

TEST_F(StrategyInstances, OneClassRunsTwiceOnAPortfolioUnderTwoNames) {
    EXPECT_EQ(se->add_strategy("SettingsProbeStrategy", "SPY"), "SettingsProbeStrategy_SPY");
    EXPECT_EQ(se->add_strategy("SettingsProbeStrategy", "SPY", {{"lookback", 30}}, "fast"), "fast");

    ASSERT_NE(probe("SettingsProbeStrategy_SPY"), nullptr);
    ASSERT_NE(probe("fast"), nullptr);
    EXPECT_EQ(probe("SettingsProbeStrategy_SPY")->lookback(), 60);
    EXPECT_EQ(probe("fast")->lookback(), 30);
    EXPECT_EQ(probe("fast")->portfolio_name(), "SPY");
    EXPECT_EQ(se->get_strategy_class_name("fast"), "SettingsProbeStrategy");
}

TEST_F(StrategyInstances, ANameInUseIsRejectedNotReplaced) {
    se->add_strategy("SettingsProbeStrategy", "SPY", {}, "s1");
    const auto* first = se->get_strategy("s1");
    EXPECT_THROW(se->add_strategy("SettingsProbeStrategy", "SPY", {{"lookback", 30}}, "s1"),
                 std::runtime_error);
    EXPECT_EQ(se->get_strategy("s1"), first);
}

TEST_F(StrategyInstances, ABadSettingAddsNothing) {
    EXPECT_THROW(se->add_strategy("SettingsProbeStrategy", "SPY", {{"threshold", 11}}),
                 std::invalid_argument);
    EXPECT_EQ(se->get_strategy("SettingsProbeStrategy_SPY"), nullptr);
}

} // namespace
