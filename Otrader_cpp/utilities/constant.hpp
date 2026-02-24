#pragma once

/**
 * C++20 equivalent of utilities/constant.py
 * General constant enums and mappings used in the trading platform.
 */

#include <string>
#include <unordered_map>

namespace utilities {

enum class Direction {
    LONG,
    SHORT,
    NET,
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
    FUTURES,
    OPTION,
    INDEX,
    FOREX,
    SPOT,
    ETF,
    BOND,
    WARRANT,
    SPREAD,
    FUND,
    CFD,
    SWAP,
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

enum class Exchange {
    SMART,
    NYSE,
    NASDAQ,
    AMEX,
    CBOE,
    IBKRATS,
    LOCAL,
};

// String conversion for enums
inline std::string to_string(Direction d) {
    switch (d) {
        case Direction::LONG: return "LONG";
        case Direction::SHORT: return "SHORT";
        case Direction::NET: return "NET";
    }
    return "LONG";
}

inline std::string to_string(Status s) {
    switch (s) {
        case Status::SUBMITTING: return "SUBMITTING";
        case Status::NOTTRADED: return "NOTTRADED";
        case Status::PARTTRADED: return "PARTTRADED";
        case Status::ALLTRADED: return "ALLTRADED";
        case Status::CANCELLED: return "CANCELLED";
        case Status::REJECTED: return "REJECTED";
    }
    return "SUBMITTING";
}

inline std::string to_string(OrderType t) {
    switch (t) {
        case OrderType::LIMIT: return "LIMIT";
        case OrderType::MARKET: return "MARKET";
    }
    return "LIMIT";
}

inline std::string to_string(OptionType t) {
    switch (t) {
        case OptionType::CALL: return "CALL";
        case OptionType::PUT: return "PUT";
    }
    return "CALL";
}

inline std::string to_string(Product p) {
    switch (p) {
        case Product::EQUITY: return "EQUITY";
        case Product::FUTURES: return "FUTURES";
        case Product::OPTION: return "OPTION";
        case Product::INDEX: return "INDEX";
        case Product::FOREX: return "FOREX";
        case Product::SPOT: return "SPOT";
        case Product::ETF: return "ETF";
        case Product::BOND: return "BOND";
        case Product::WARRANT: return "WARRANT";
        case Product::SPREAD: return "SPREAD";
        case Product::FUND: return "FUND";
        case Product::CFD: return "CFD";
        case Product::SWAP: return "SWAP";
        case Product::UNKNOWN: return "UNKNOWN";
    }
    return "UNKNOWN";
}

inline std::string to_string(ComboType t) {
    switch (t) {
        case ComboType::CUSTOM: return "custom";
        case ComboType::SPREAD: return "spread";
        case ComboType::STRADDLE: return "straddle";
        case ComboType::STRANGLE: return "strangle";
        case ComboType::DIAGONAL_SPREAD: return "diagonal_spread";
        case ComboType::RATIO_SPREAD: return "ratio_spread";
        case ComboType::RISK_REVERSAL: return "risk_reversal";
        case ComboType::BUTTERFLY: return "butterfly";
        case ComboType::INVERSE_BUTTERFLY: return "inverse_butterfly";
        case ComboType::IRON_CONDOR: return "iron_condor";
        case ComboType::IRON_BUTTERFLY: return "iron_butterfly";
        case ComboType::CONDOR: return "condor";
        case ComboType::BOX_SPREAD: return "box_spread";
    }
    return "custom";
}

inline std::string to_string(Exchange e) {
    switch (e) {
        case Exchange::SMART: return "SMART";
        case Exchange::NYSE: return "NYSE";
        case Exchange::NASDAQ: return "NASDAQ";
        case Exchange::AMEX: return "AMEX";
        case Exchange::CBOE: return "CBOE";
        case Exchange::IBKRATS: return "IBKRATS";
        case Exchange::LOCAL: return "LOCAL";
    }
    return "LOCAL";
}

// Active statuses (order still in progress)
inline bool is_active_status(Status s) {
    return s == Status::SUBMITTING || s == Status::NOTTRADED || s == Status::PARTTRADED;
}

// Other constants (from constant.py)
inline constexpr const char* JOIN_SYMBOL = "-";

}  // namespace utilities
