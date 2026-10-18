#include "core/bookkeeping/bookkeeping_engine.hpp"
#include "model/constant.hpp"
#include "model/object.hpp"
#include "model/portfolio.hpp"
#include "model/symbol.hpp"
#include "utilities/logger.hpp"
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <gtest/gtest.h>
#include <tuple>
#include <vector>
 
namespace {
 
using model::Side;
using model::Fill;
 
static Fill make_trade(std::string order_id, std::string fill_id, std::string symbol,
                       Side dir, double price, double quantity) {
    Fill t;
    t.order_id = std::move(order_id);
    t.fill_id = std::move(fill_id);
    t.symbol = std::move(symbol);
    t.side = dir;
    t.price = price;
    t.quantity = quantity;
    return t;
}
 
/** The fill booked as the strategy's, its order (as given, or of its contract alone) done. */
void book(core::BookkeepingEngine& engine, const std::string& strategy, const Fill& fill,
          const model::Order* order) {
    if (engine.find_order(fill.order_id) == nullptr) {
        model::Order sent;
        if (order != nullptr) {
            sent = *order;
        } else {
            sent.symbol = fill.symbol;
            sent.legs = {{.ratio = 1, .side = fill.side, .symbol = fill.symbol}};
        }
        sent.order_id = fill.order_id;
        sent.status = model::Status::FILLED;
        engine.add_order(strategy, sent);
    }
    engine.book_fill(fill);
}

/** An order over these legs, each with its side (one of each a unit). */
static model::Order combo_order(std::string id,
                                std::vector<std::pair<std::string, Side>> legs) {
    model::Order o;
    o.order_id = std::move(id);
    o.combo_type = model::ComboType::CUSTOM;
    for (auto& [symbol, side] : legs) {
        model::Leg l;
        l.symbol = symbol;
        l.side = side;
        l.ratio = 1;
        o.symbol += (o.symbol.empty() ? "combo_" : "|") + symbol;
        o.legs.push_back(l);
    }
    return o;
}

TEST(BookkeepingHoldings, UnderlyingAvgCostAndRealizedPnlOnCloseAndReverse) {
    core::BookkeepingEngine pe;
    const std::string strat = "s1";
    auto buy10_100 = make_trade("OID", "TID-1", "AAPL", Side::BUY, 100.0, 10);
    auto buy10_110 = make_trade("OID", "TID-2", "AAPL", Side::BUY, 110.0, 10);
    book(pe, strat, buy10_100, nullptr);
    book(pe, strat, buy10_110, nullptr);
 
    // Avg cost = (10*100 + 10*110)/20 = 105.00
    auto& h1 = pe.ensure_holding(strat);
    EXPECT_EQ(h1.shares.quantity, 20);
    EXPECT_DOUBLE_EQ(h1.shares.avg_cost, 105.0);
 
    // Sell 5 @120 => realized = (120-105)*5 = 75
    auto sell5_120 = make_trade("OID", "TID-3", "AAPL", Side::SELL, 120.0, 5);
    book(pe, strat, sell5_120, nullptr);
    auto& h2 = pe.ensure_holding(strat);
    EXPECT_EQ(h2.shares.quantity, 15);
    EXPECT_DOUBLE_EQ(h2.shares.realized_pnl, 75.0);
    EXPECT_DOUBLE_EQ(h2.shares.avg_cost, 105.0);
 
    // Sell 25 @90 closes remaining 15 (loss) then opens short 10 @90
    // Close PnL = (90-105)*15 = -225
    auto sell25_90 = make_trade("OID", "TID-4", "AAPL", Side::SELL, 90.0, 25);
    book(pe, strat, sell25_90, nullptr);
    auto& h3 = pe.ensure_holding(strat);
    EXPECT_EQ(h3.shares.quantity, -10);
    EXPECT_DOUBLE_EQ(h3.shares.avg_cost, 90.0);
    EXPECT_DOUBLE_EQ(h3.shares.realized_pnl, 75.0 - 225.0);
}
 
TEST(BookkeepingHoldings, AFillIsAnOptionWhenItsSymbolNamesOne) {
    core::BookkeepingEngine pe;
    const std::string strat = "s1";

    book(pe, strat, make_trade("OID", "TID-1", "SPY", Side::BUY, 580.0, 3), nullptr);
    book(pe, 
         strat, make_trade("OID", "TID-2", "SPY-20991218-P-600", Side::BUY, 2.0, 1), nullptr);

    const auto& h = pe.ensure_holding(strat);
    EXPECT_EQ(h.shares.quantity, 3);
    EXPECT_FALSE(h.options.contains("SPY"));
    EXPECT_TRUE(h.options.contains("SPY-20991218-P-600"));
}

TEST(BookkeepingHoldings, SingleLegOptionUsesMultiplierAndRealizedPnl) {
    core::BookkeepingEngine pe;
    const std::string strat = "s1";
 
    // Buy 2 contracts @1.00, then sell 1 @1.50 => realized = (1.5-1.0)*1*100 = 50
    auto b2 = make_trade("OID", "TID-1", "SPXW-20250804-C-5000", Side::BUY, 1.00, 2);
    book(pe, strat, b2, nullptr);
    auto s1 = make_trade("OID", "TID-2", "SPXW-20250804-C-5000", Side::SELL, 1.50, 1);
    book(pe, strat, s1, nullptr);
 
    const auto& h = pe.ensure_holding(strat);
    auto it = h.options.find("SPXW-20250804-C-5000");
    ASSERT_NE(it, h.options.end());
    EXPECT_EQ(it->second.quantity, 1);
    EXPECT_DOUBLE_EQ(it->second.avg_cost, 1.00);
    EXPECT_DOUBLE_EQ(it->second.multiplier, 100.0);
    EXPECT_DOUBLE_EQ(it->second.realized_pnl, 50.0);
}
 
static model::Order make_combo_order(std::string order_id, std::string symbol,
                                     model::ComboType type,
                                     std::vector<std::string> const& leg_symbols) {
    model::Order o;
    o.order_id = std::move(order_id);
    o.symbol = std::move(symbol);
    o.combo_type = type;
    std::vector<model::Leg> legs;
    int con_id = 1;
    for (auto const& sym : leg_symbols) {
        model::Leg l;
        l.con_id = con_id++;
        l.ratio = 1;
        l.side = Side::BUY;
        l.symbol = sym;
        legs.push_back(l);
    }
    o.legs = legs;
    return o;
}

// A straddle opened by one order and closed by another of another type over the same legs: flat
// in each contract, each still saying what it was opened in.
TEST(BookkeepingHoldings, AStraddleClosedByAnotherOrderIsFlatInEachContract) {
    core::BookkeepingEngine pe;
    const std::string strat = "s1";
    const std::string call = "SPXW-20250804-C-6325";
    const std::string put = "SPXW-20250804-P-6325";

    auto open = make_combo_order("OID-1", "straddle_SIG", model::ComboType::STRADDLE,
                                 {call, put});
    book(pe, strat, make_trade("OID-1", "T1-0", call, Side::BUY, 3.50, 1),
                           &open);
    book(pe, strat, make_trade("OID-1", "T1-1", put, Side::BUY, 27.70, 1),
                           &open);

    auto close =
        make_combo_order("OID-2", "custom_SIG", model::ComboType::CUSTOM, {put, call});
    book(pe, strat, make_trade("OID-2", "T2-0", put, Side::SELL, 24.60, 1),
                           &close);
    book(pe, strat, make_trade("OID-2", "T2-1", call, Side::SELL, 3.90, 1),
                           &close);

    const auto& h = pe.ensure_holding(strat);
    EXPECT_TRUE(h.options.empty());
    ASSERT_EQ(h.closed_options.size(), 2U);
    double realized = 0.0;
    for (const auto& [symbol, pos] : h.closed_options) {
        EXPECT_EQ(pos.quantity, 0) << symbol;
        EXPECT_EQ(pos.combo_type, model::ComboType::STRADDLE) << symbol;
        realized += pos.realized_pnl;
    }
    EXPECT_TRUE(h.is_flat());
    // (24.60 - 27.70 + 3.90 - 3.50) * 100
    EXPECT_NEAR(realized, -270.0, 1e-9);
    EXPECT_NEAR(h.closed_realized_pnl, -270.0, 1e-9);
}

// cost_value is signed like current_value(): a short's premium is a credit, so
// unrealized = current_value() - cost_value is right for both directions.
TEST(BookkeepingHoldings, ShortCostValueIsSignedSoUnrealizedPnlIsCorrect) {
    core::BookkeepingEngine pe;
    const std::string strat = "s1";
    const std::string sym = "SPXW-20250804-C-5000";
    book(pe, strat, make_trade("OID", "T1", sym, Side::SELL, 2.00, 1), nullptr);

    auto& pos = pe.ensure_holding(strat).options.at(sym);
    EXPECT_EQ(pos.quantity, -1);
    EXPECT_DOUBLE_EQ(pos.cost_value, -200.0);
    pos.mid_price = 1.50; // option fell: the short made 50
    EXPECT_DOUBLE_EQ(pos.current_value() - pos.cost_value, 50.0);
}

/** A combo order fills as its contracts: each held by itself, opened in that combo (its legs'
 * symbols, sorted). */
TEST(BookkeepingHoldings, AComboIsHeldAsItsContracts) {
    core::BookkeepingEngine pe;
    const std::string strat = "s";
    const std::string call = "SPY-20991218-C-600";
    const std::string put = "SPY-20991218-P-600";
    const std::string low = "SPY-20991218-C-601";
    const std::string high = "SPY-20991218-C-603";
    const auto held = [&](const std::string& symbol) {
        return pe.ensure_holding(strat).options.at(symbol);
    };

    // Two straddles bought: two of each.
    const auto straddle =
        combo_order("1", {{call, Side::BUY}, {put, Side::BUY}});
    book(pe, strat, make_trade("1", "1a", call, Side::BUY, 5.0, 2), &straddle);
    book(pe, strat, make_trade("1", "1b", put, Side::BUY, 4.0, 2), &straddle);
    EXPECT_EQ(held(call).quantity, 2);
    EXPECT_EQ(held(put).quantity, 2);
    EXPECT_EQ(held(put).combo, call + "|" + put);

    // A call spread sold: the low call sold, the high one bought.
    const auto spread =
        combo_order("2", {{low, Side::SELL}, {high, Side::BUY}});
    book(pe, strat, make_trade("2", "2a", low, Side::SELL, 1.0, 1), &spread);
    book(pe, strat, make_trade("2", "2b", high, Side::BUY, 0.4, 1), &spread);
    EXPECT_EQ(held(low).quantity, -1);
    EXPECT_EQ(held(high).quantity, 1);
    EXPECT_EQ(held(low).combo, low + "|" + high);

    // The straddle closed by another combo order over the same legs.
    const auto closing =
        combo_order("3", {{call, Side::SELL}, {put, Side::SELL}});
    book(pe, strat, make_trade("3", "3a", call, Side::SELL, 6.0, 2), &closing);
    book(pe, strat, make_trade("3", "3b", put, Side::SELL, 3.0, 2), &closing);
    const auto& h = pe.ensure_holding(strat);
    EXPECT_EQ(h.closed_options.at(call).quantity, 0);
    EXPECT_EQ(h.closed_options.at(put).quantity, 0);
    EXPECT_EQ(h.options.size(), 2U); // the spread
}

/** The same fills in another order (the closing order's first, legs interleaved) leave the same
 * holding. */
TEST(BookkeepingHoldings, HoldingIsIndependentOfFillOrder) {
    const std::string call = "SPY-20991218-C-600";
    const std::string put = "SPY-20991218-P-600";
    const auto open =
        combo_order("1", {{call, Side::BUY}, {put, Side::BUY}});
    const auto close =
        combo_order("2", {{call, Side::SELL}, {put, Side::SELL}});
    const std::vector<std::pair<Fill, const model::Order*>> fills{
        {make_trade("1", "1a", call, Side::BUY, 5.0, 3), &open},
        {make_trade("1", "1b", put, Side::BUY, 4.0, 3), &open},
        {make_trade("2", "2a", call, Side::SELL, 6.0, 1), &close},
        {make_trade("2", "2b", put, Side::SELL, 3.0, 1), &close},
    };
    const auto held_after = [&](std::vector<int> sequence) {
        core::BookkeepingEngine pe;
        for (int i : sequence) {
            book(pe, "s", fills[i].first, fills[i].second);
        }
        const auto& positions = pe.ensure_holding("s").options;
        return std::tuple{positions.at(call).quantity, positions.at(put).quantity,
                          positions.at(call).cost_value, positions.at(call).combo,
                          positions.size()};
    };
    const auto expected = held_after({0, 1, 2, 3});
    EXPECT_EQ(std::get<0>(expected), 2);
    EXPECT_EQ(held_after({2, 3, 0, 1}), expected) << "the closing order's fills first";
    EXPECT_EQ(held_after({3, 0, 2, 1}), expected) << "legs interleaved";
}

// A call bought above its mid, then the market moves a day on: the fill's cost against the mid,
// then the move by the Greeks the position had, with what they do not explain as other.
TEST(BookkeepingHoldings, PnlIsAttributedToItsSourcesAndAddsUp) {
    using namespace std::chrono;
    core::BookkeepingEngine pe;
    const std::string strat = "s1";
    const std::string call = model::option_symbol({.root = "SPY",
                                                   .expiry = 2099y / December / 18,
                                                   .type = model::OptionType::CALL,
                                                   .strike = 600});
    model::Portfolio portfolio("SPY");
    model::Contract spy;
    spy.symbol = "SPY";
    spy.multiplier = 1.0;
    portfolio.set_underlying(spy);
    model::Contract option;
    option.symbol = call;
    option.multiplier = 100.0;
    portfolio.list(std::make_shared<const model::Listing>(std::vector{option}));
    const auto mark = [&](double underlying, double mid, double iv, sys_days day) {
        model::PortfolioSnapshot s = portfolio.snapshot();
        s.datetime = day;
        s.underlying_mark = underlying;
        s.values(&model::Frame::bid)[0] = mid;
        s.values(&model::Frame::ask)[0] = mid;
        s.values(&model::Frame::iv)[0] = iv;
        s.values(&model::Frame::delta)[0] = 0.5; // per option: 50 a contract
        s.values(&model::Frame::gamma)[0] = 0.02;
        s.values(&model::Frame::theta)[0] = -0.05; // 5 USD a day a contract
        s.values(&model::Frame::vega)[0] = 0.2;    // 20 USD a vol point a contract
        s.priced = true;
        portfolio.apply(s);
        pe.value_holding(strat, portfolio);
    };

    pe.ensure_holding(strat);
    mark(600.0, 10.0, 0.20, sys_days{2099y / January / 5}); // the first update: nothing held
    book(pe, strat, make_trade("OID", "T1", call, Side::BUY, 10.5, 2), nullptr);
    mark(600.0, 10.0, 0.20, sys_days{2099y / January / 5});
    const model::HoldingSummary& first = pe.ensure_holding(strat).summary;
    EXPECT_DOUBLE_EQ(first.pnl, -100.0);
    EXPECT_DOUBLE_EQ(first.pnl_fills, -100.0); // 2 x (10.0 - 10.5) x 100

    mark(602.0, 11.2, 0.21, sys_days{2099y / January / 6});
    const model::HoldingSummary& s = pe.ensure_holding(strat).summary;
    EXPECT_NEAR(s.pnl_delta, 200.0, 1e-9); // 2 x 50 x 2
    EXPECT_NEAR(s.pnl_gamma, 8.0, 1e-9);   // 0.5 x 2 x 2 x 2^2
    EXPECT_NEAR(s.pnl_theta, -10.0, 1e-9); // 2 x -5 x 1 day
    EXPECT_NEAR(s.pnl_vega, 40.0, 1e-9);   // 2 x 20 x 1 point
    EXPECT_NEAR(s.pnl_other, 2.0, 1e-9);   // 2 x 1.2 x 100 = 240, 238 explained
    EXPECT_NEAR(s.pnl_fills, -100.0, 1e-9);
    EXPECT_NEAR(s.pnl_delta + s.pnl_gamma + s.pnl_theta + s.pnl_vega + s.pnl_other + s.pnl_fills,
                s.pnl, 1e-9);
    EXPECT_NEAR(s.pnl, 140.0, 1e-9);
}

/** A fill of no leg of its combo order, or without its order, opens its contract as alone. */
TEST(BookkeepingHoldings, AFillOutsideItsCombosLegsOpensItsContractAlone) {
    core::BookkeepingEngine pe;
    const std::string call = "SPY-20991218-C-600";
    const std::string put = "SPY-20991218-P-600";
    const std::string stray = "SPY-20991218-C-650";
    const auto straddle =
        combo_order("1", {{call, Side::BUY}, {put, Side::BUY}});
    book(pe, "s", make_trade("1", "1a", stray, Side::BUY, 1.0, 1), &straddle);
    book(pe, "s", make_trade("2", "2a", call, Side::BUY, 5.0, 2), nullptr);

    const auto& h = pe.ensure_holding("s");
    EXPECT_EQ(h.options.at(stray).quantity, 1);
    EXPECT_EQ(h.options.at(stray).combo, stray);
    EXPECT_EQ(h.options.at(stray).combo_type, model::ComboType::SINGLE_LEG);
    EXPECT_EQ(h.options.at(call).quantity, 2);
    EXPECT_EQ(h.options.at(call).combo, call);
    EXPECT_EQ(pe.held_options(), (std::vector<std::string>{call, stray}));
}

// Sold as a one-leg combo, bought back as a plain order: one position, flat.
TEST(BookkeepingHoldings, AOneLegComboAndAPlainOrderOfItsOptionShareOnePosition) {
    core::BookkeepingEngine pe;
    const std::string call = "SPXW-20991218-C-7735";
    const auto single = combo_order("1", {{call, Side::SELL}});
    book(pe, "s", make_trade("1", "1a", call, Side::SELL, 19.4, 1), &single);
    EXPECT_EQ(pe.ensure_holding("s").options.at(call).quantity, -1);
    book(pe, "s", make_trade("2", "2a", call, Side::BUY, 19.3, 1), nullptr);

    const auto& h = pe.ensure_holding("s");
    EXPECT_TRUE(h.is_flat());
    EXPECT_TRUE(h.options.empty());
    EXPECT_EQ(h.closed_options.size(), 1U);
    EXPECT_TRUE(pe.held_options().empty());
}

TEST(BookkeepingHoldings, HoldsOnlyWhatIsOpenSharesOptionsAndComboLegs) {
    core::BookkeepingEngine pe;
    const std::string call = "SPY-20991218-C-600";
    const std::string put = "SPY-20991218-P-600";
    const auto straddle =
        combo_order("1", {{call, Side::BUY}, {put, Side::BUY}});
    book(pe, "s", make_trade("1", "1a", call, Side::BUY, 5.0, 1), &straddle);
    book(pe, "s", make_trade("2", "2a", "SPY", Side::BUY, 600.0, 10), nullptr);

    EXPECT_TRUE(pe.holds("s", call)); // a combo's leg
    EXPECT_FALSE(pe.holds("s", put)); // its other leg, not filled
    EXPECT_TRUE(pe.holds("s", "SPY"));
    EXPECT_FALSE(pe.holds("other", call));
    book(pe, "s", make_trade("3", "3a", "SPY", Side::SELL, 601.0, 10), nullptr);
    EXPECT_FALSE(pe.holds("s", "SPY")); // sold
}

struct Recorder final : utilities::Logger {
    void log(const utilities::LogRecord& record) override { records.push_back(record); }
    std::vector<utilities::LogRecord> records;
};

/** The account page checks on each refresh: a check is logged only when its outcome changes. */
TEST(BookkeepingHoldings, TheBrokerCheckIsLoggedWhenItsOutcomeChanges) {
    Recorder logger;
    core::BookkeepingEngine pe(&logger);
    EXPECT_EQ(pe.broker_check().first, "not yet");
    const std::vector<core::BrokerCheckRow> agree{
        {.symbol = "AAPL", .engine = 0, .broker = 5, .traded_here = false},
        {.symbol = "SPY", .engine = 10, .broker = 10, .traded_here = true}};
    pe.record_broker_check(agree);
    ASSERT_EQ(logger.records.size(), 2U);
    EXPECT_NE(logger.records[0].msg.find("not traded here: 5 AAPL"), std::string::npos);
    EXPECT_NE(logger.records[1].msg.find("agree"), std::string::npos);
    EXPECT_EQ(pe.broker_check(), (std::pair<std::string, std::string>{"agree", ""}));
    pe.record_broker_check(agree);
    EXPECT_EQ(logger.records.size(), 2U); // as last time

    const std::vector<core::BrokerCheckRow> short_two{
        {.symbol = "SPY", .engine = 10, .broker = 8, .traded_here = true}};
    pe.record_broker_check(short_two);
    ASSERT_EQ(logger.records.size(), 3U);
    EXPECT_EQ(logger.records[2].level, utilities::ERROR);
    EXPECT_EQ(pe.broker_check(),
              (std::pair<std::string, std::string>{"disagree", "SPY: engine 10, broker 8"}));
    pe.record_broker_check(short_two);
    EXPECT_EQ(logger.records.size(), 3U);
    const std::vector<core::BrokerCheckRow> short_three{
        {.symbol = "SPY", .engine = 10, .broker = 7, .traded_here = true}};
    pe.record_broker_check(short_three); // another problem: logged again
    EXPECT_EQ(logger.records.size(), 4U);
    pe.record_broker_check({});
    EXPECT_EQ(pe.broker_check().first, "agree");
}

TEST(BookkeepingHoldings, APartCloseOfAShortKeepsItsSideAndItsCost) {
    core::BookkeepingEngine pe;
    book(pe, "s", make_trade("1", "1", "SPY", Side::SELL, 100.0, 5), nullptr);
    book(pe, "s", make_trade("2", "2", "SPY", Side::BUY, 90.0, 2), nullptr);
    const auto& shares = pe.ensure_holding("s").shares;
    EXPECT_EQ(shares.quantity, -3);
    EXPECT_DOUBLE_EQ(shares.avg_cost, 100.0);
    EXPECT_DOUBLE_EQ(shares.cost_value, -300.0);
    EXPECT_DOUBLE_EQ(shares.realized_pnl, 20.0); // (100 - 90) x 2
}

/** What a contract was opened in is set as it opens from flat, by the order that opened it. */
TEST(BookkeepingHoldings, WhatAContractWasOpenedInIsSetAsItOpensFromFlat) {
    core::BookkeepingEngine pe;
    const std::string call = "SPY-20991218-C-600";
    const std::string put = "SPY-20991218-P-600";
    const std::string wing = "SPY-20991218-C-650";
    const auto& positions = pe.ensure_holding("s").options;
    const auto& closed = pe.ensure_holding("s").closed_options;

    auto straddle = make_combo_order("1", "straddle_SIG", model::ComboType::STRADDLE, {call, put});
    book(pe, "s", make_trade("1", "1a", call, Side::BUY, 5.0, 1), &straddle);
    book(pe, "s", make_trade("1", "1b", put, Side::BUY, 4.0, 1), &straddle);

    const auto close_call = combo_order("2", {{call, Side::SELL}});
    book(pe, "s", make_trade("2", "2a", call, Side::SELL, 6.0, 1), &close_call);
    EXPECT_EQ(closed.at(call).quantity, 0);
    EXPECT_EQ(positions.at(put).combo, call + "|" + put) << "the put is held as it was opened";

    const auto three = combo_order(
        "3", {{call, Side::BUY}, {wing, Side::BUY}, {put, Side::BUY}});
    book(pe, "s", make_trade("3", "3a", call, Side::BUY, 5.0, 1), &three);
    book(pe, "s", make_trade("3", "3b", put, Side::BUY, 4.0, 1), &three);
    EXPECT_FALSE(closed.contains(call));
    EXPECT_EQ(positions.at(call).combo, call + "|" + wing + "|" + put) << "opened again: by this";
    EXPECT_EQ(positions.at(call).combo_type, model::ComboType::CUSTOM);
    EXPECT_EQ(positions.at(put).quantity, 2);
    EXPECT_EQ(positions.at(put).combo, call + "|" + put) << "added to: as it was opened";
}

TEST(BookkeepingHoldings, SharesAreMarkedWithoutAQuoteAndTheirRealizedPnlCountsWhenFlat) {
    core::BookkeepingEngine pe;
    model::Portfolio bare("SPY"); // no underlying quoted
    model::PortfolioSnapshot at = bare.snapshot();
    at.datetime = std::chrono::sys_days{std::chrono::year{2099} / 1 / 5};
    bare.apply(at);
    pe.value_holding("nobody", bare);
    EXPECT_EQ(pe.find_holding("nobody"), nullptr);

    book(pe, "s", make_trade("1", "1", "SPY", Side::BUY, 100.0, 10), nullptr);
    pe.value_holding("s", bare);
    const model::HoldingSummary& held = pe.ensure_holding("s").summary;
    EXPECT_DOUBLE_EQ(held.delta, 10.0); // a share is a unit of delta
    EXPECT_DOUBLE_EQ(held.current_value, 0.0);

    book(pe, "s", make_trade("2", "2", "SPY", Side::SELL, 105.0, 10), nullptr);
    pe.value_holding("s", bare);
    const model::HoldingSummary& flat = pe.ensure_holding("s").summary;
    EXPECT_DOUBLE_EQ(flat.realized_pnl, 50.0);
    EXPECT_DOUBLE_EQ(flat.pnl, 50.0);
    EXPECT_DOUBLE_EQ(flat.delta, 0.0);
}

TEST(BookkeepingHoldings, PnlOfSharesIsTheirDeltaAndAnUnmarkedOptionsIsTheFills) {
    using namespace std::chrono;
    const model::DateTime day = sys_days{2099y / January / 5};
    core::BookkeepingEngine pe;
    model::Portfolio spy("SPY");
    model::Contract contract;
    contract.symbol = "SPY";
    spy.set_underlying(contract);
    /** The portfolio quoted at `day`: the underlying at `underlying`, its option (if any) at
     * `mid` with this IV. */
    const auto quote = [&](model::Portfolio& p, double underlying, double mid, double iv) {
        model::PortfolioSnapshot s = p.snapshot();
        s.datetime = day;
        s.underlying_mark = underlying;
        if (p.listing().size() > 0) {
            s.values(&model::Frame::bid)[0] = mid;
            s.values(&model::Frame::ask)[0] = mid;
            s.values(&model::Frame::iv)[0] = iv;
        }
        s.priced = true;
        p.apply(s);
    };
    quote(spy, 0.0, 0.0, 0.0);
    pe.ensure_holding("s");
    pe.value_holding("s", spy); // the baseline
    book(pe, "s", make_trade("1", "1", "SPY", Side::BUY, 600.0, 10), nullptr);
    quote(spy, 600.0, 0.0, 0.0);
    pe.value_holding("s", spy);
    quote(spy, 602.0, 0.0, 0.0);
    pe.value_holding("s", spy);
    const model::HoldingSummary& shares = pe.ensure_holding("s").summary;
    EXPECT_NEAR(shares.pnl_delta, 20.0, 1e-9);
    EXPECT_NEAR(shares.pnl_fills, 0.0, 1e-9);
    EXPECT_NEAR(shares.pnl, 20.0, 1e-9);

    // A call with no implied vol at the start and no underlying quote: no move, no vol change.
    core::BookkeepingEngine options;
    model::Portfolio unquoted("SPY");
    const std::string call = "SPY-20991218-C-600";
    model::Contract option;
    option.symbol = call;
    option.multiplier = 100.0;
    unquoted.list(std::make_shared<const model::Listing>(std::vector{option}));
    quote(unquoted, 0.0, 0.0, 0.0);
    options.ensure_holding("s");
    options.value_holding("s", unquoted);
    book(options, "s", make_trade("1", "1", call, Side::BUY, 10.0, 1),
                                nullptr);
    quote(unquoted, 0.0, 10.0, 0.0);
    options.value_holding("s", unquoted);
    quote(unquoted, 0.0, 11.0, 0.2);
    options.value_holding("s", unquoted);
    const model::HoldingSummary& s = options.ensure_holding("s").summary;
    EXPECT_NEAR(s.pnl_delta, 0.0, 1e-9);
    EXPECT_NEAR(s.pnl_vega, 0.0, 1e-9);
    EXPECT_NEAR(s.pnl_other, 100.0, 1e-9);
    EXPECT_NEAR(s.pnl_fills, 0.0, 1e-9);

    quote(unquoted, 0.0, 0.0, 0.2); // no mark now: its change comes in with the fills
    options.value_holding("s", unquoted);
    EXPECT_NEAR(s.pnl, -1000.0, 1e-9);
    EXPECT_NEAR(s.pnl_other, 100.0, 1e-9);
    EXPECT_NEAR(s.pnl_fills, -1100.0, 1e-9);
}

TEST(BookkeepingHoldings, AStartWaitsForExpiredOptionsSettledAndTheBrokerAgreeing) {
    core::BookkeepingEngine pe;
    const model::DateTime now = std::chrono::sys_days{std::chrono::year{2099} / 1 / 5};
    const auto refusal = [&](bool required) { return pe.start_refusal("s", now, required); };
    EXPECT_EQ(refusal(false), std::nullopt);
    ASSERT_TRUE(refusal(true));
    EXPECT_TRUE(refusal(true)->starts_with("Connect first")) << *refusal(true);

    pe.record_broker_check_failed("no answer");
    EXPECT_EQ(pe.broker_check(),
              (std::pair<std::string, std::string>{"failed", "no answer"}));
    ASSERT_TRUE(refusal(true));
    EXPECT_NE(refusal(true)->find("were not checked against the holdings (no answer)"),
              std::string::npos);

    const std::vector<core::BrokerCheckRow> short_two{
        {.symbol = "SPY", .engine = 10, .broker = 8, .traded_here = true}};
    pe.record_broker_check(short_two);
    ASSERT_TRUE(refusal(true));
    EXPECT_NE(refusal(true)->find("disagree with the holdings (SPY: engine 10, broker 8)"),
              std::string::npos);
    EXPECT_EQ(refusal(false), std::nullopt);
    pe.record_broker_check({});
    EXPECT_EQ(refusal(true), std::nullopt);

    book(pe, "s", make_trade("1", "1", "SPY-20990102-C-600", Side::BUY, 1.0, 1),
                           nullptr);
    ASSERT_TRUE(refusal(false)); // expired, not settled: whatever the broker says
    EXPECT_NE(refusal(false)->find("expired options not settled (SPY-20990102-C-600)"),
              std::string::npos);
}

TEST(BookkeepingHoldings, SharesOfTheHedgeCountInTheUnderlyingsUnits) {
    model::Portfolio spxw("SPXW");
    model::Contract spx;
    spx.symbol = "SPX";
    model::Contract spy;
    spy.symbol = "SPY";
    spxw.set_underlying(spx);
    spxw.set_hedge_instrument(spy);
    model::PortfolioSnapshot quotes = spxw.snapshot();
    quotes.underlying_mark = 6000.0;
    quotes.hedge_mark = 600.0;
    spxw.apply(quotes);

    core::BookkeepingEngine pe;
    book(pe, "s", make_trade("1", "1", "SPY", Side::SELL, 600.0, 500), nullptr);
    pe.value_holding("s", spxw);

    const model::StrategyHolding& h = pe.ensure_holding("s");
    EXPECT_EQ(h.shares.symbol, "SPY"); // named by its fill
    EXPECT_EQ(h.shares.quantity, -500);
    EXPECT_DOUBLE_EQ(h.summary.delta, -50.0);           // in SPX units
    EXPECT_DOUBLE_EQ(h.summary.current_value, -300000); // at SPY's price
    EXPECT_DOUBLE_EQ(h.summary.pnl, 0.0);
}

} // namespace
