/**
 * A strategy runs against any StrategyContext. Here a fake one records what the strategy asks of
 * it, and this test links the strategy layer alone: no engine, so none can sneak back in.
 */
#include "strategy/strategy_context.hpp"
#include "strategy/template.hpp"
#include <format>
#include <gtest/gtest.h>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

class FakeContext final : public strategy::StrategyContext {
  public:
    FakeContext() : portfolio_("SPY") {
        model::ContractData spy;
        spy.symbol = "SPY";
        spy.product = model::Product::EQUITY;
        portfolio_.set_underlying(spy);
    }

    const model::PortfolioData* get_portfolio(const std::string& name) const override {
        return name == portfolio_.name ? &portfolio_ : nullptr;
    }
    const model::ContractData* get_contract(const std::string& /*symbol*/) const override {
        return nullptr;
    }
    bool has_active_order(const std::string& /*strategy_name*/) const override { return false; }

    std::vector<std::string> send_order(const std::string& strategy_name, const std::string& symbol,
                                        model::Direction direction, double /*price*/,
                                        double volume,
                                        model::OrderType /*order_type*/) override {
        orders.push_back(std::format("{} {} {} {}", strategy_name, symbol,
                                     model::to_string(direction), volume));
        return {std::to_string(orders.size())};
    }
    std::vector<std::string>
    send_combo_order(const std::string& /*strategy_name*/, model::ComboType /*combo_type*/,
                     const std::string& /*combo_sig*/, model::Direction /*direction*/,
                     double /*price*/, double /*volume*/, std::span<const model::Leg> /*legs*/,
                     model::OrderType /*order_type*/) override {
        return {};
    }
    void register_hedging(const std::string& strategy_name, const std::string& /*portfolio_name*/,
                          int /*timer_trigger*/, int /*delta_target*/,
                          int /*delta_range*/) override {
        hedged.push_back(strategy_name);
    }
    void unregister_hedging(const std::string& /*strategy_name*/) override {}
    void subscribe_chains(const std::string& /*strategy_name*/,
                          std::span<const std::string> chains) override {
        subscribed.assign(chains.begin(), chains.end());
    }
    void write_log(const std::string& /*strategy_name*/, const std::string& msg,
                   utilities::LogLevel /*level*/) const override {
        logs.push_back(msg);
    }

    std::vector<std::string> orders;
    std::vector<std::string> hedged;
    std::vector<std::string> subscribed;
    mutable std::vector<std::string> logs;

  private:
    model::PortfolioData portfolio_;
};

class Probe final : public strategy::OptionStrategyTemplate {
  public:
    using OptionStrategyTemplate::OptionStrategyTemplate;

    void on_init_logic() override {
        subscribe_chains(std::vector<std::string>{"SPY_20991218"});
        register_hedging();
    }
    void on_stop_logic() override {}
    void on_timer_logic() override {}
};

TEST(StrategyContext, AStrategyActsThroughItsContextAlone) {
    FakeContext context;
    Probe probe(&context, "probe", "SPY", {{"timer_trigger", 1}});
    probe.on_init();
    EXPECT_EQ(context.subscribed, std::vector<std::string>{"SPY_20991218"});
    EXPECT_EQ(context.hedged, std::vector<std::string>{"probe"});

    EXPECT_EQ(probe.underlying_order(model::Direction::LONG, 0.0, 2.0),
              std::vector<std::string>{"1"});
    EXPECT_EQ(context.orders, std::vector<std::string>{"probe SPY LONG 2"});
    EXPECT_FALSE(context.logs.empty()); // it says it was created, through the context
}

TEST(StrategyContext, AStrategyNeedsItsPortfolio) {
    FakeContext context;
    EXPECT_THROW(Probe(&context, "probe", "QQQ", {{"timer_trigger", 1}}), std::runtime_error);
    EXPECT_THROW(Probe(nullptr, "probe", "SPY", {{"timer_trigger", 1}}), std::runtime_error);
}

} // namespace
