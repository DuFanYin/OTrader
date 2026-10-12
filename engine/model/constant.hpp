#pragma once

/**
 * C++23 equivalent of utilities/constant.py
 * General constant enums and mappings used in the trading platform.
 */

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <unordered_map>

namespace model {

enum class Direction {
    LONG,
    SHORT,
};

enum class Status {
    SUBMITTING,
    NOTTRADED,
    PARTTRADED,
    ALLTRADED,
    CANCELLED,
    REJECTED,
};

enum class Product {
    EQUITY,
    OPTION,
    INDEX,
    UNKNOWN,
};

enum class OrderType {
    LIMIT,
    MARKET,
};

enum class OptionType {
    CALL,
    PUT,
};

enum class ComboType {
    SINGLE_LEG,
    CUSTOM,
    SPREAD,
    STRADDLE,
    STRANGLE,
    DIAGONAL_SPREAD,
    RATIO_SPREAD,
    RISK_REVERSAL,
    BUTTERFLY,
    INVERSE_BUTTERFLY,
    IRON_CONDOR,
    IRON_BUTTERFLY,
    CONDOR,
    BOX_SPREAD,
};


// String conversion for enums
inline std::string to_string(Direction d) {
    switch (d) {
    case Direction::LONG:
        return "LONG";
    case Direction::SHORT:
        return "SHORT";
    }
    return "LONG";
}

inline std::string to_string(Status s) {
    switch (s) {
    case Status::SUBMITTING:
        return "SUBMITTING";
    case Status::NOTTRADED:
        return "NOTTRADED";
    case Status::PARTTRADED:
        return "PARTTRADED";
    case Status::ALLTRADED:
        return "ALLTRADED";
    case Status::CANCELLED:
        return "CANCELLED";
    case Status::REJECTED:
        return "REJECTED";
    }
    return "SUBMITTING";
}

inline std::string to_string(OrderType t) {
    switch (t) {
    case OrderType::LIMIT:
        return "LIMIT";
    case OrderType::MARKET:
        return "MARKET";
    }
    return "LIMIT";
}

inline std::string to_string(OptionType t) {
    switch (t) {
    case OptionType::CALL:
        return "CALL";
    case OptionType::PUT:
        return "PUT";
    }
    return "CALL";
}

inline std::string to_string(Product p) {
    switch (p) {
    case Product::EQUITY:
        return "EQUITY";
    case Product::OPTION:
        return "OPTION";
    case Product::INDEX:
        return "INDEX";
    case Product::UNKNOWN:
        return "UNKNOWN";
    }
    return "UNKNOWN";
}

inline std::string to_string(ComboType t) {
    switch (t) {
    case ComboType::SINGLE_LEG:
        return "single_leg";
    case ComboType::CUSTOM:
        return "custom";
    case ComboType::SPREAD:
        return "spread";
    case ComboType::STRADDLE:
        return "straddle";
    case ComboType::STRANGLE:
        return "strangle";
    case ComboType::DIAGONAL_SPREAD:
        return "diagonal_spread";
    case ComboType::RATIO_SPREAD:
        return "ratio_spread";
    case ComboType::RISK_REVERSAL:
        return "risk_reversal";
    case ComboType::BUTTERFLY:
        return "butterfly";
    case ComboType::INVERSE_BUTTERFLY:
        return "inverse_butterfly";
    case ComboType::IRON_CONDOR:
        return "iron_condor";
    case ComboType::IRON_BUTTERFLY:
        return "iron_butterfly";
    case ComboType::CONDOR:
        return "condor";
    case ComboType::BOX_SPREAD:
        return "box_spread";
    }
    return "custom";
}

// from_string for JSON deserialization
inline Direction from_string_direction(const std::string& s) {
    if (s == "SHORT")
        return Direction::SHORT;
    return Direction::LONG;
}
inline Status from_string_status(const std::string& s) {
    if (s == "NOTTRADED")
        return Status::NOTTRADED;
    if (s == "PARTTRADED")
        return Status::PARTTRADED;
    if (s == "ALLTRADED")
        return Status::ALLTRADED;
    if (s == "CANCELLED")
        return Status::CANCELLED;
    if (s == "REJECTED")
        return Status::REJECTED;
    return Status::SUBMITTING;
}
inline Product from_string_product(const std::string& s) {
    if (s == "EQUITY")
        return Product::EQUITY;
    if (s == "OPTION")
        return Product::OPTION;
    if (s == "INDEX")
        return Product::INDEX;
    return Product::UNKNOWN;
}
inline OrderType from_string_ordertype(const std::string& s) {
    if (s == "MARKET")
        return OrderType::MARKET;
    return OrderType::LIMIT;
}
/** The ComboType named by to_string() or by its enum name, in either case ("straddle",
 * "STRADDLE"); CUSTOM for any other. */
inline ComboType from_string_combo(std::string_view s) {
    for (int i = 0; i <= static_cast<int>(ComboType::BOX_SPREAD); ++i) {
        const auto t = static_cast<ComboType>(i);
        const std::string name = to_string(t);
        if (std::ranges::equal(name, s, [](char a, char b) {
                return a == std::tolower(static_cast<unsigned char>(b));
            })) {
            return t;
        }
    }
    return ComboType::CUSTOM;
}

// Active statuses (order still in progress)
inline bool is_active_status(Status s) {
    return s == Status::SUBMITTING || s == Status::NOTTRADED || s == Status::PARTTRADED;
}

} // namespace model
