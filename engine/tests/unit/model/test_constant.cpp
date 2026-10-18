/**
 * The enums as text and back: every value reads back as itself (the database and the gRPC API carry
 * them as text), and text that names none reads as the default.
 */
#include "model/constant.hpp"
#include <gtest/gtest.h>

namespace {

using namespace model;

TEST(Constant, EveryValueReadsBackAsItself) {
    for (const Side s : {Side::BUY, Side::SELL}) {
        EXPECT_EQ(from_string_side(to_string(s)), s);
    }
    for (const Status s : {Status::SUBMITTING, Status::WORKING, Status::PARTLY_FILLED,
                           Status::FILLED, Status::CANCELLED, Status::REJECTED}) {
        EXPECT_EQ(from_string_status(to_string(s)), s);
    }
    for (const Product p : {Product::EQUITY, Product::OPTION, Product::INDEX, Product::UNKNOWN}) {
        EXPECT_EQ(from_string_product(to_string(p)), p);
    }
    for (const OrderType t : {OrderType::LIMIT, OrderType::MARKET}) {
        EXPECT_EQ(from_string_ordertype(to_string(t)), t);
    }
    for (int i = 0; i <= static_cast<int>(ComboType::BOX_SPREAD); ++i) {
        const auto t = static_cast<ComboType>(i);
        EXPECT_EQ(from_string_combo(to_string(t)), t) << to_string(t);
    }
    EXPECT_EQ(to_string(OptionType::CALL), "CALL");
    EXPECT_EQ(to_string(OptionType::PUT), "PUT");
}

TEST(Constant, TextNamingNoValueReadsAsTheDefault) {
    EXPECT_EQ(from_string_side("sell"), Side::BUY); // the names are upper case
    EXPECT_EQ(from_string_status(""), Status::SUBMITTING);
    EXPECT_EQ(from_string_product("FUTURE"), Product::UNKNOWN);
    EXPECT_EQ(from_string_ordertype("STOP"), OrderType::LIMIT);
    EXPECT_EQ(from_string_combo("IRON_BUTTERFLY"), ComboType::IRON_BUTTERFLY); // either case
    EXPECT_EQ(from_string_combo("iron fly"), ComboType::CUSTOM);
}

TEST(Constant, OnlyAnOrderStillInProgressIsActive) {
    EXPECT_TRUE(is_active_status(Status::SUBMITTING));
    EXPECT_TRUE(is_active_status(Status::WORKING));
    EXPECT_TRUE(is_active_status(Status::PARTLY_FILLED));
    EXPECT_FALSE(is_active_status(Status::FILLED));
    EXPECT_FALSE(is_active_status(Status::CANCELLED));
    EXPECT_FALSE(is_active_status(Status::REJECTED));
}

} // namespace
