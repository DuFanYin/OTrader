/**
 * The platform's instrument names: one per instrument, built and taken apart only by
 * utilities/symbol, the same in backtest and live.
 */
#include "model/symbol.hpp"
#include <chrono>
#include <gtest/gtest.h>

namespace {

using namespace std::chrono;
using model::OptionId;
using model::OptionType;

TEST(Symbol, AnOptionIsRootExpiryRightStrike) {
    const OptionId id{
        .root = "SPXW", .expiry = 2026y / October / 16, .type = OptionType::PUT, .strike = 5800};
    EXPECT_EQ(model::option_symbol(id), "SPXW-20261016-P-5800");
    EXPECT_EQ(model::parse_option_symbol("SPXW-20261016-P-5800"), id);
    EXPECT_EQ(model::chain_symbol("SPXW", 2026y / October / 16), "SPXW_20261016");
}

TEST(Symbol, HalfStrikesKeepTheirOwnName) {
    EXPECT_EQ(model::strike_text(582.5), "582.5");
    EXPECT_EQ(model::strike_text(582.0), "582");
    const OptionId half{.root = "SPY", .expiry = 2026y / October / 16, .strike = 582.5};
    OptionId whole = half;
    whole.strike = 582;
    EXPECT_NE(model::option_symbol(half), model::option_symbol(whole));
    EXPECT_EQ(model::parse_option_symbol(model::option_symbol(half)), half);
}

TEST(Symbol, OnlyOptionsParse) {
    for (const char* other : {"SPY", "SPY-20261016-C", "SPY-2026101-C-580", "SPY-20261332-C-580",
                              "SPY-20261016-X-580", "SPY-20261016-C-0", "SPY-20261016-C-58x",
                              "-20261016-C-580", "SPY-20261016-C-580-100-USD-OPT"}) {
        EXPECT_FALSE(model::parse_option_symbol(other)) << other;
    }
}

} // namespace
