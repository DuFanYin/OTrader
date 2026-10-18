/** Combo legs (model::combo): each combo type's legs by role, sized per combo unit, the combo's
 * side on the legs it buys and the opposite on the rest; the legs that close a held combo. */
#include "model/order_builder.hpp"
#include "model/constant.hpp"
#include "model/object.hpp"
#include "model/symbol.hpp"
#include <algorithm>
#include <chrono>
#include <gtest/gtest.h>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

using namespace std::chrono;
using model::ComboType;
using model::Side;
using model::Leg;
using model::OrderType;

auto opposite(Side d) -> Side {
    return d == Side::BUY ? Side::SELL : Side::BUY;
}

/** A leg by its role: whether it takes the combo's side, and its contracts per unit. */
struct Role {
    std::string name;
    bool buys;
    int ratio = 1;
};

struct Shape {
    ComboType type;
    std::vector<Role> roles;
};

const std::vector<Shape> kShapes = {
    {ComboType::STRADDLE, {{"call", true}, {"put", true}}},
    {ComboType::STRANGLE, {{"call", true}, {"put", true}}},
    {ComboType::SPREAD, {{"long_leg", true}, {"short_leg", false}}},
    {ComboType::DIAGONAL_SPREAD, {{"long_leg", true}, {"short_leg", false}}},
    {ComboType::RISK_REVERSAL, {{"long_leg", true}, {"short_leg", false}}},
    {ComboType::RATIO_SPREAD, {{"long_leg", true}, {"short_leg", false, 2}}},
    {ComboType::IRON_CONDOR,
     {{"put_lower", false}, {"put_upper", true}, {"call_lower", true}, {"call_upper", false}}},
    {ComboType::BUTTERFLY, {{"body", true}, {"wing1", false}, {"wing2", false}}},
    {ComboType::INVERSE_BUTTERFLY, {{"body", false}, {"wing1", true}, {"wing2", true}}},
    {ComboType::IRON_BUTTERFLY, {{"put_wing", true}, {"body", false}, {"call_wing", true}}},
    {ComboType::CONDOR,
     {{"long_put", true}, {"short_put", false}, {"short_call", false}, {"long_call", true}}},
    {ComboType::BOX_SPREAD,
     {{"long_call", true}, {"short_call", false}, {"short_put", false}, {"long_put", true}}},
};

/** Options and their contracts, by symbol; find_contract finds them. */
class ComboBuilder : public ::testing::Test {
  protected:
    /** An SPXW option at this strike (a call above 5000, else a put), with its contract. */
    auto add(double strike) -> std::string {
        const auto type = strike > 5000 ? model::OptionType::CALL : model::OptionType::PUT;
        const std::string symbol = model::option_symbol(
            {.root = "SPXW", .expiry = year{2099} / December / 18, .type = type, .strike = strike});
        model::Contract& c = contracts_[symbol];
        c.symbol = symbol;
        c.con_id = static_cast<int>(strike);
        c.trading_class = "SPXW";
        return symbol;
    }

    std::unordered_map<std::string, model::Contract> contracts_;
    const model::ComboGetContractFn get_contract_ =
        [this](const std::string& symbol) -> const model::Contract* {
        const auto it = contracts_.find(symbol);
        return it != contracts_.end() ? &it->second : nullptr;
    };
};

TEST_F(ComboBuilder, EachComboTypesLegsTakeTheirRolesSidesAndRatios) {
    for (const Shape& shape : kShapes) {
        std::unordered_map<std::string, std::string> by_role;
        std::vector<std::string> symbols;
        double strike = 5010;
        for (const Role& role : shape.roles) {
            by_role[role.name] = add(strike);
            symbols.push_back(by_role[role.name]);
            strike += 10;
        }
        std::ranges::sort(symbols);
        std::string signature;
        for (const std::string& s : symbols) {
            signature += (signature.empty() ? "" : "|") + s;
        }
        for (const Side side : {Side::BUY, Side::SELL}) {
            const std::string what = model::to_string(side) + " combo type " +
                                     std::to_string(static_cast<int>(shape.type));
            const auto [legs, sig] =
                model::build_combo(by_role, shape.type, side, get_contract_);
            ASSERT_EQ(legs.size(), shape.roles.size()) << what;
            for (std::size_t i = 0; i < legs.size(); ++i) {
                const Role& role = shape.roles[i];
                const std::string& symbol = by_role.at(role.name);
                EXPECT_EQ(legs[i].symbol, symbol) << what << " " << role.name;
                EXPECT_EQ(legs[i].side, role.buys ? side : opposite(side))
                    << what << " " << role.name;
                EXPECT_EQ(legs[i].ratio, role.ratio) << what << " " << role.name;
                EXPECT_EQ(legs[i].con_id, contracts_.at(symbol).con_id.value()) << what;
                EXPECT_EQ(legs[i].trading_class, "SPXW") << what;
                EXPECT_FALSE(legs[i].price.has_value()) << what;
            }
            EXPECT_EQ(sig, signature) << what;
        }
    }
}

TEST_F(ComboBuilder, ARiskReversalBuysItsLongLegAndSellsItsShortLegAsASpreadDoes) {
    const std::unordered_map<std::string, std::string> by_role = {
        {"long_leg", add(5050)}, {"short_leg", add(4950)}};
    for (const Side side : {Side::BUY, Side::SELL}) {
        const auto [rr, rr_sig] = model::build_combo(by_role, ComboType::RISK_REVERSAL,
                                                     side, get_contract_);
        const auto [spread, spread_sig] =
            model::build_combo(by_role, ComboType::SPREAD, side, get_contract_);
        ASSERT_EQ(rr.size(), 2U);
        EXPECT_EQ(rr[0].symbol, by_role.at("long_leg"));
        EXPECT_EQ(rr[0].side, side);
        EXPECT_EQ(rr[1].symbol, by_role.at("short_leg"));
        EXPECT_EQ(rr[1].side, opposite(side));
        ASSERT_EQ(spread.size(), 2U);
        for (std::size_t i = 0; i < 2; ++i) {
            EXPECT_EQ(rr[i].symbol, spread[i].symbol);
            EXPECT_EQ(rr[i].side, spread[i].side);
            EXPECT_EQ(rr[i].ratio, spread[i].ratio);
        }
        EXPECT_EQ(rr_sig, spread_sig);
    }
}

TEST_F(ComboBuilder, ASingleLegAndACustomComboTakeTheCombosDirection) {
    const std::string call = add(5050);
    const auto [one, one_sig] = model::build_combo({{"any", call}}, ComboType::SINGLE_LEG,
                                                   Side::SELL, get_contract_);
    ASSERT_EQ(one.size(), 1U);
    EXPECT_EQ(one[0].symbol, call);
    EXPECT_EQ(one[0].side, Side::SELL);
    EXPECT_EQ(one[0].ratio, 1);
    EXPECT_EQ(one_sig, call);

    const std::string put = add(4950);
    const auto [custom, custom_sig] = model::build_combo(
        {{"a", call}, {"b", put}}, ComboType::CUSTOM, Side::BUY, get_contract_);
    ASSERT_EQ(custom.size(), 2U);
    EXPECT_TRUE(std::ranges::all_of(custom, [](const Leg& l) {
        return l.side == Side::BUY && l.ratio == 1;
    }));
    EXPECT_EQ(custom_sig, std::min(call, put) + "|" +
                              std::max(call, put));
}

TEST_F(ComboBuilder, AComboMissingARoleOrAContractIsRefused) {
    const std::string call = add(5050);
    const std::string put = add(4950);
    const auto build = [this](const std::unordered_map<std::string, std::string>& o,
                              ComboType type) {
        return model::build_combo(o, type, Side::BUY, get_contract_);
    };
    EXPECT_THROW(build({}, ComboType::SINGLE_LEG), std::runtime_error);
    EXPECT_THROW(build({{"a", call}, {"b", put}}, ComboType::SINGLE_LEG), std::runtime_error);
    EXPECT_THROW(build({{"call", call}}, ComboType::STRADDLE), std::runtime_error);
    EXPECT_THROW(build({{"put", put}}, ComboType::STRADDLE), std::runtime_error);
    EXPECT_THROW(build({{"call", call}}, ComboType::STRANGLE), std::runtime_error);
    EXPECT_THROW(build({{"put", put}}, ComboType::STRANGLE), std::runtime_error);

    EXPECT_THROW(build({{"any", "SPXW-20991218-C-6000"}}, ComboType::SINGLE_LEG),
                 std::runtime_error); // not listed
}

TEST(OrderRequest, OneLegAtRatioOneIsItsContractsOwnOrder) {
    model::Contract call;
    call.symbol = "SPY-20991218-C-600";
    call.con_id = 7;
    call.trading_class = "SPY";
    const std::vector legs{model::contract_leg(call, Side::SELL)};
    const auto limit = model::order_request(legs, -1.234, 2.4, OrderType::LIMIT);
    EXPECT_FALSE(limit.is_combo());
    EXPECT_EQ(limit.symbol, call.symbol);
    EXPECT_EQ(limit.combo_type, ComboType::SINGLE_LEG);
    EXPECT_EQ(limit.legs.front().con_id, 7);
    EXPECT_EQ(limit.legs.front().side, Side::SELL);
    EXPECT_DOUBLE_EQ(limit.price, -1.23); // received, to the cent
    EXPECT_EQ(limit.quantity, 2.0);         // whole units
    EXPECT_EQ(limit.trading_class, "SPY");
    EXPECT_EQ(model::order_request(legs, -1.234, 1, OrderType::MARKET).price, 0.0);
}

TEST(OrderRequest, AnythingElseIsAComboNamedByItsTypeAndItsLegs) {
    const std::vector<model::Leg> legs{
        {.ratio = 1, .side = Side::BUY, .symbol = "SPY-20991218-P-600"},
        {.ratio = 1, .side = Side::BUY, .symbol = "SPY-20991218-C-600",
         .trading_class = "SPY"}};
    const auto req = model::order_request(legs, 0.556, 3, OrderType::LIMIT, ComboType::STRADDLE);
    EXPECT_TRUE(req.is_combo());
    EXPECT_EQ(req.combo_type, ComboType::STRADDLE);
    EXPECT_EQ(req.symbol, model::to_string(ComboType::STRADDLE) +
                              "_SPY-20991218-C-600|SPY-20991218-P-600");
    EXPECT_DOUBLE_EQ(req.price, 0.56);
    EXPECT_EQ(req.quantity, 3.0);

    const std::vector two_of_one{model::Leg{.ratio = 2, .symbol = "SPY-20991218-C-600"}};
    EXPECT_TRUE(model::order_request(two_of_one, 1.0, 1, OrderType::LIMIT, ComboType::CUSTOM)
                    .is_combo()) << "one leg at ratio 2: a combo";
}

TEST_F(ComboBuilder, AComboMissingAnyOfItsRolesIsRefused) {
    const std::string low = add(4900);
    const std::string mid = add(4950);
    const std::string high = add(5050);
    EXPECT_THROW(model::build_combo({{"put_lower", low}, {"put_upper", mid}, {"call_lower", high}},
                                    ComboType::IRON_CONDOR, Side::BUY, get_contract_),
                 std::exception); // no call_upper
    EXPECT_THROW(model::build_combo({{"long_leg", low}}, ComboType::SPREAD, Side::BUY,
                                    get_contract_),
                 std::exception); // no short_leg
}

TEST_F(ComboBuilder, ALegNeedsAWayToFindItsContract) {
    const std::string call = add(5050);
    EXPECT_THROW(model::create_leg(call, Side::BUY, 1, {}), std::runtime_error);
}

TEST_F(ComboBuilder, ASignatureNamesItsOptionLegsOnly) {
    const std::string call = add(5050);
    const std::vector<Leg> legs{{.ratio = 100, .side = Side::SELL, .symbol = "SPX"},
                                model::create_leg(call, Side::BUY, 1, get_contract_)};
    EXPECT_EQ(model::generate_combo_signature(legs), call);
}

} // namespace
