/**
 * A strategy runs against any StrategyContext. Here a fake one records what the strategy asks of
 * it, and this test links the strategy layer alone: no engine, so none can sneak back in.
 */
#include "strategy/strategy_context.hpp"
#include "model/session_calendar.hpp"
#include "strategy/strategy.hpp"
#include "quant/engine_api.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <chrono>
#include <format>
#include <gtest/gtest.h>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

class FakeContext final : public strategy::StrategyContext {
  public:
    FakeContext() : portfolio_("SPY") {
        model::Contract spy;
        spy.symbol = "SPY";
        spy.product = model::Product::EQUITY;
        portfolio_.set_underlying(spy);
    }

    const model::StrategyRecord& record() const override { return record_; }
    const model::Portfolio& portfolio() const override { return portfolio_; }
    const model::StrategyHolding& holding() const override { return held; }
    bool has_active_order() const override { return false; }
    const model::DailyTable& daily() const override { return table; }

    std::string send_order(const model::OrderRequest& request) override {
        if (request.is_combo()) {
            combos.push_back(model::generate_combo_signature(request.legs));
            return "combo " + std::to_string(combos.size());
        }
        orders.push_back(std::format("{} {} {} {}", record_.name, request.symbol,
                                     model::to_string(request.legs.front().side),
                                     request.quantity));
        return std::to_string(orders.size());
    }
    void register_hedging(int /*timer_trigger*/, int /*delta_target*/,
                          int /*delta_range*/) override {
        hedged = true;
    }
    void unregister_hedging() override { hedged = false; }
    void subscribe_chains(std::span<const std::string> chains) override {
        chains_subscribed.assign(chains.begin(), chains.end());
    }
    bool subscribed(std::string_view chain) const override {
        return std::ranges::contains(chains_subscribed, chain);
    }
    void write_log(const std::string& msg, utilities::LogLevel /*level*/) const override {
        logs.push_back(msg);
    }
    void set_error(const std::string& msg) override { errors.push_back(msg); }
    void write_value(std::string_view name, double value) override {
        values.insert_or_assign(std::string(name), value);
    }

    model::DailyTable table;

    /** Its one portfolio, SPY. */
    model::Portfolio& spy() { return portfolio_; }
    /** Its quotes now: at `time`, the underlying at `spot`. */
    void quote(model::DateTime time, double spot = 0.0) {
        model::PortfolioSnapshot s = portfolio_.snapshot();
        s.datetime = time;
        s.underlying_mark = spot;
        s.priced = true;
        portfolio_.apply(s);
    }

    model::StrategyHolding held;
    std::vector<std::string> orders;
    std::vector<std::string> combos;
    bool hedged = false;
    std::vector<std::string> chains_subscribed;
    std::vector<std::string> errors;
    mutable std::vector<std::string> logs;
    std::map<std::string, double> values;

  private:
    model::StrategyRecord record_{.name = "probe", .setting = {{"timer_trigger", 1}}};
    model::Portfolio portfolio_;
};

class Probe final : public strategy::Strategy {
  public:
    using Strategy::Strategy;

    void on_init() override {
        subscribe_chains(std::vector<std::string>{"SPY_20991218"});
        register_hedging();
    }
    void on_timer() override {}
};

TEST(StrategyContext, AStrategyActsThroughItsContextAlone) {
    FakeContext context;
    Probe probe(context);
    probe.on_init();
    EXPECT_EQ(context.chains_subscribed, std::vector<std::string>{"SPY_20991218"});
    EXPECT_TRUE(context.hedged);

    EXPECT_EQ(probe.send_order(std::array{probe.leg("SPY", model::Side::BUY)}, 0.0, 2.0),
              "1");
    EXPECT_EQ(context.orders, std::vector<std::string>{"probe SPY BUY 2"});
    probe.set_error("broke");
    EXPECT_EQ(context.errors, std::vector<std::string>{"broke"});
}

/** Does nothing on its own. */
class Idle final : public strategy::Strategy {
  public:
    using Strategy::Strategy;

    void on_timer() override {}
};


auto spy_option(model::OptionType type, double strike) -> std::string {
    using namespace std::chrono;
    return model::option_symbol(
        {.root = "SPY", .expiry = 2099y / December / 18, .type = type, .strike = strike});
}

/** Calls at 590, 600 and 610, a put at 600 only, listed in its portfolio. */
void list_options(FakeContext& context) {
    std::vector<model::Contract> options;
    for (const auto& [type, strike] : {std::pair{model::OptionType::CALL, 590.0},
                                       std::pair{model::OptionType::CALL, 600.0},
                                       std::pair{model::OptionType::CALL, 610.0},
                                       std::pair{model::OptionType::PUT, 600.0}}) {
        model::Contract c;
        c.symbol = spy_option(type, strike);
        c.multiplier = 100;
        options.push_back(c);
    }
    context.spy().list(std::make_shared<const model::Listing>(options));
}

/** Reads a parameter its class does not declare. */
class Undeclared final : public strategy::Strategy {
  public:
    using Strategy::Strategy;

    void on_timer() override {}

  private:
    struct Setting {
        strategy::Param<int> lookback{"lookback", 60, 2, 390};
    };
    const Setting setting_ = read_setting<Setting>();
};

TEST(Strategy, AParameterItsClassDoesNotDeclareCannotBeRead) {
    FakeContext context;
    EXPECT_THROW(Undeclared{context}, std::logic_error);
}

TEST(Strategy, AChainOrAStrikeNotThereIsNone) {
    using namespace std::chrono;
    FakeContext context;
    list_options(context);
    Idle probe(context);
    EXPECT_TRUE(probe.select_chain(0, 5).empty()); // decades away
    const std::string chain = probe.select_chain(0, 100'000);
    ASSERT_EQ(chain, "SPY_20991218");
    EXPECT_EQ(context.chains_subscribed, std::vector<std::string>{chain});
    EXPECT_FALSE(probe.find_chain("SPY_20991219")); // not subscribed
    probe.subscribe_chains(std::vector<std::string>{"SPY_20991219"});
    EXPECT_FALSE(probe.find_chain("SPY_20991219")); // subscribed, not listed
    EXPECT_FALSE(probe.find_chain(chain));          // listed, no longer subscribed

    const model::Chain listed = *context.spy().chain(chain);
    const auto at = [&](model::OptionType type, int steps) -> std::string {
        const std::optional<model::Option> o = probe.option_at(listed, type, steps);
        return o ? o->symbol() : "none";
    };
    EXPECT_EQ(at(model::OptionType::CALL, 0), spy_option(model::OptionType::CALL, 600)); // middle
    context.quote(sys_days{2099y / March / 2} + 15h, 609.0);
    EXPECT_EQ(at(model::OptionType::CALL, 0), spy_option(model::OptionType::CALL, 610));
    context.quote(sys_days{2099y / March / 2} + 15h, 601.0);
    EXPECT_EQ(at(model::OptionType::CALL, 0), spy_option(model::OptionType::CALL, 600));
    EXPECT_EQ(at(model::OptionType::CALL, 1), spy_option(model::OptionType::CALL, 610));
    EXPECT_EQ(at(model::OptionType::CALL, -1), spy_option(model::OptionType::CALL, 590));
    EXPECT_EQ(at(model::OptionType::CALL, 2), "none");  // past the last strike
    EXPECT_EQ(at(model::OptionType::CALL, -2), "none"); // before the first
    EXPECT_EQ(at(model::OptionType::PUT, 0), spy_option(model::OptionType::PUT, 600));
    EXPECT_EQ(at(model::OptionType::PUT, 1), "none"); // no put listed there
}

TEST(Strategy, AnOrderIsNotSentWithoutWhatItNeeds) {
    FakeContext context;
    list_options(context);
    Idle probe(context);
    EXPECT_FALSE(probe.holds_options()); // no holding
    probe.close_all_strategy_positions();
    EXPECT_TRUE(probe.send_order(std::array{probe.leg("DIA", model::Side::BUY)})
                    .empty()); // not listed
    EXPECT_NE(context.logs.back().find("No contract DIA"), std::string::npos);

    const std::string call = spy_option(model::OptionType::CALL, 600);
    const std::string put = spy_option(model::OptionType::PUT, 600);
    EXPECT_TRUE(probe.combo_legs(model::ComboType::SINGLE_LEG, {{"a", call}, {"b", put}},
                                 model::Side::BUY)
                    .empty()); // one option only
    EXPECT_NE(context.logs.back().find("No single_leg"), std::string::npos);
    EXPECT_TRUE(probe.send_order({}).empty());
    EXPECT_TRUE(context.orders.empty());
    EXPECT_TRUE(context.combos.empty());

    const std::vector two_of_one{probe.leg(call, model::Side::SELL, 2)};
    EXPECT_EQ(probe.send_order(two_of_one), "combo 1"); // not the option's own
    EXPECT_EQ(context.combos, std::vector<std::string>{call});
}

TEST(Strategy, ClosingSendsWhatCanBeClosedAndLogsWhatCannot) {
    FakeContext context;
    list_options(context);
    Idle probe(context);
    model::StrategyHolding& holding = context.held;
    holding.shares.symbol = "SPY";
    holding.shares.quantity = -5;
    model::OptionPosition unlisted(spy_option(model::OptionType::CALL, 999), 100.0);
    unlisted.quantity = 1;
    holding.options.insert_or_assign(unlisted.symbol, unlisted);

    probe.close_all_strategy_positions();
    EXPECT_EQ(context.orders, std::vector<std::string>{"probe SPY BUY 5"}); // shares bought back

    // Options one of which is not listed: not sent, and said so.
    std::vector<model::OptionPosition> options{
        model::OptionPosition(spy_option(model::OptionType::CALL, 600), 100.0),
        model::OptionPosition(spy_option(model::OptionType::PUT, 999), 100.0)};
    options[0].quantity = 1;
    options[1].quantity = 1;
    probe.close_options(options);
    EXPECT_TRUE(context.combos.empty());
    EXPECT_NE(context.logs.back().find("No contract"), std::string::npos);
}

} // namespace

namespace {

TEST(StrategyContext, ValuesByDayAreAsOfTheDayBefore) {
    using namespace std::chrono;
    FakeContext context;
    context.table.set("SPY", {{2025y / March / 3, 100.0}, {2025y / March / 4, 101.0},
                              {2025y / March / 5, 99.0}, {2025y / March / 6, 500.0}});
    context.table.set("FOMC", {{2025y / March / 19, 1.0}});
    context.quote(model::kNyse.at(2025y / March / 6, 10h)); // Thursday, 10:00
    context.spy().set_trading_day(sys_days{2025y / March / 6});
    Idle s(context);
    EXPECT_EQ(s.daily("SPY"), 99.0); // Wednesday's close, not today's
    EXPECT_EQ(s.daily_history("SPY", 2), (std::vector<double>{101.0, 99.0}));
    EXPECT_EQ(s.trading_days_until("FOMC"), 9); // Mar 6, 7, 10 to 14, 17, 18
    EXPECT_EQ(s.trading_days_until("CPI"), -1);
    EXPECT_EQ(s.daily("VIX"), std::nullopt);
    const std::optional<double> vol = s.realized_vol("SPY", 2);
    ASSERT_TRUE(vol.has_value());
    EXPECT_GT(*vol, 0.0);
    EXPECT_EQ(s.rank_in("SPY", 100.0, 3), 2.0 / 3.0); // 100 and 99 of 100, 101, 99
}

TEST(StrategyContext, ASeriesNoLongerKeptIsMissingNotItsLastValue) {
    using namespace std::chrono;
    FakeContext context;
    context.table.set("FIVE", {{2025y / February / 26, 1.0}}); // Feb 27, 28, Mar 3, 4, 5 missed
    context.table.set("SIX", {{2025y / February / 25, 2.0}});  // and Feb 26: one too many
    context.quote(model::kNyse.at(2025y / March / 6, 10h));
    context.spy().set_trading_day(sys_days{2025y / March / 6});
    Idle s(context);
    EXPECT_EQ(s.daily("FIVE"), 1.0);
    EXPECT_EQ(s.daily("SIX"), std::nullopt);
    EXPECT_TRUE(s.daily_history("SIX", 3).empty());
}

TEST(StrategyContext, TheSkewAndTheTermSlopeComeFromTheChainsNow) {
    // IVs at |delta| 0.25 interpolated in each wing; the risk reversal is the call's less the put's.
    EXPECT_DOUBLE_EQ(quant::iv_at_delta(std::array{0.4, 0.2, 0.1}, std::array{0.20, 0.24, 0.30}, 0.25), 0.23);
    EXPECT_DOUBLE_EQ(quant::iv_at_delta(std::array{-0.4, -0.2}, std::array{0.22, 0.30}, 0.25), 0.28);
    EXPECT_TRUE(std::isnan(quant::iv_at_delta(std::array{0.4, 0.3}, std::array{0.2, 0.2}, 0.25)));
    EXPECT_DOUBLE_EQ(quant::percentile_rank(std::array{1.0, 2.0, 3.0, 4.0}, 2.5), 0.5);
    EXPECT_NEAR(quant::realized_vol(std::array{100.0, 101.0, 100.0}, 1.0),
                std::abs(std::log(101.0 / 100.0) - std::log(100.0 / 101.0)) / std::sqrt(2.0), 1e-12);
}

} // namespace
