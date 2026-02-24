#pragma once

/**
 * C++20 equivalent of utilities/object.py
 * Basic data structures for the trading platform.
 */

#include "constant.hpp"
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace utilities {

using DateTime = std::chrono::system_clock::time_point;

// ------------------------------ Base ------------------------------

struct BaseData {
    std::string gateway_name;
};

/** Log message for EVENT_LOG (level: DEBUG=10, INFO=20, WARNING=30, ERROR=40, CRITICAL=50). */
struct LogData : BaseData {
    std::string msg;
    int level = 20;  // INFO
    std::string time;  // formatted e.g. "MM-DD HH:mm:ss"
};

// ------------------------------ Market Data ------------------------------

struct TickData : BaseData {
    std::string symbol;
    Exchange exchange = Exchange::LOCAL;
    DateTime datetime{};

    std::string name;
    double volume = 0;
    double turnover = 0;
    double open_interest = 0;
    double last_price = 0;
    double last_volume = 0;
    double bid_price_1 = 0;
    double ask_price_1 = 0;
    std::optional<DateTime> localtime;
};

struct OptionMarketData : BaseData {
    std::string symbol;
    Exchange exchange = Exchange::LOCAL;
    DateTime datetime{};

    double bid_price = 0.0;
    double ask_price = 0.0;
    double last_price = 0.0;
    double volume = 0.0;
    double open_interest = 0.0;
    double delta = 0.0;
    double gamma = 0.0;
    double theta = 0.0;
    double vega = 0.0;
    double mid_iv = 0.0;
};

struct ChainMarketData : BaseData {
    std::string chain_symbol;
    DateTime datetime{};
    std::string underlying_symbol;
    double underlying_bid = 0.0;
    double underlying_ask = 0.0;
    double underlying_last = 0.0;
    std::unordered_map<std::string, OptionMarketData> options;

    void add_option(const OptionMarketData& option_data);
};

/** Compact portfolio snapshot (plan B): fixed order = portfolio option_apply_order. No per-option symbol/datetime. */
struct PortfolioSnapshot {
    std::string portfolio_name;
    DateTime datetime{};
    double underlying_bid = 0.0;
    double underlying_ask = 0.0;
    double underlying_last = 0.0;
    /** Option values in portfolio option_apply_order; length = total option count. */
    std::vector<double> bid;
    std::vector<double> ask;
    std::vector<double> last;
    std::vector<double> delta;
    std::vector<double> gamma;
    std::vector<double> theta;
    std::vector<double> vega;
    std::vector<double> iv;
};

// ------------------------------ Contract ------------------------------

struct ContractData : BaseData {
    std::string symbol;
    Exchange exchange = Exchange::LOCAL;
    std::string name;
    Product product = Product::UNKNOWN;
    double size = 1.0;
    double pricetick = 0.01;

    double min_volume = 1;
    std::optional<double> max_volume;
    bool stop_supported = false;
    bool net_position = false;
    bool history_data = false;
    std::optional<int> con_id;
    std::optional<std::string> trading_class;

    std::optional<double> option_strike;
    std::optional<std::string> option_underlying;
    std::optional<OptionType> option_type;
    std::optional<DateTime> option_listed;
    std::optional<DateTime> option_expiry;
    std::optional<std::string> option_portfolio;
    std::optional<std::string> option_index;
};

// ------------------------------ Order / Trade ------------------------------

struct Leg : BaseData {
    int con_id = 0;
    Exchange exchange = Exchange::LOCAL;
    int ratio = 0;
    Direction direction = Direction::LONG;
    std::optional<double> price;
    std::optional<std::string> symbol;
    std::optional<std::string> trading_class;
};

struct TradeData : BaseData {
    std::string symbol;
    Exchange exchange = Exchange::LOCAL;
    std::string orderid;
    std::string tradeid;
    std::optional<Direction> direction;
    double price = 0;
    double volume = 0;
    std::optional<DateTime> datetime;
};

struct OrderData : BaseData {
    std::string symbol;
    Exchange exchange = Exchange::LOCAL;
    std::string orderid;
    std::optional<std::string> trading_class;

    OrderType type = OrderType::LIMIT;
    std::optional<Direction> direction;
    double price = 0;
    double volume = 0;
    double traded = 0;
    Status status = Status::SUBMITTING;
    std::optional<DateTime> datetime;
    std::string reference;

    bool is_combo = false;
    std::optional<std::vector<Leg>> legs;
    std::optional<ComboType> combo_type;

    [[nodiscard]] bool is_active() const;
    struct CancelRequest create_cancel_request() const;
};

struct OrderRequest {
    std::string symbol;
    Exchange exchange = Exchange::SMART;
    Direction direction = Direction::LONG;
    OrderType type = OrderType::LIMIT;
    double volume = 0;
    double price = 0;
    std::string reference;
    std::optional<std::string> trading_class;
    bool is_combo = false;
    std::optional<std::vector<Leg>> legs;
    std::optional<ComboType> combo_type;

    OrderData create_order_data(const std::string& orderid, const std::string& gateway_name) const;
};

struct CancelRequest {
    std::string orderid;
    std::string symbol;
    Exchange exchange = Exchange::LOCAL;
    bool is_combo = false;
    std::optional<std::vector<Leg>> legs;
};

// ------------------------------ Position Holding ------------------------------

struct BasePosition {
    std::string symbol;
    int quantity = 0;
    double avg_cost = 0.0;
    double cost_value = 0.0;
    double realized_pnl = 0.0;
    double mid_price = 0.0;
    double delta = 0.0;
    double gamma = 0.0;
    double theta = 0.0;
    double vega = 0.0;
    double multiplier = 1.0;

    [[nodiscard]] double current_value() const;
    void clear_fields();
};

struct OptionPositionData : BasePosition {
    OptionPositionData() { multiplier = 100.0; }
    OptionPositionData(const std::string& sym) : BasePosition{sym} { multiplier = 100.0; }
};

struct UnderlyingPositionData : BasePosition {
    UnderlyingPositionData() : BasePosition{"Underlying"} { delta = 1.0; }
};

struct ComboPositionData : BasePosition {
    ComboType combo_type = ComboType::CUSTOM;
    std::vector<OptionPositionData> legs;

    ComboPositionData() { multiplier = 100.0; }
    ComboPositionData(const std::string& sym) : BasePosition{sym} { multiplier = 100.0; }
    void clear_fields();
};

struct PortfolioSummary {
    double total_cost = 0.0;
    double current_value = 0.0;
    double unrealized_pnl = 0.0;
    double realized_pnl = 0.0;
    double pnl = 0.0;
    double delta = 0.0;
    double gamma = 0.0;
    double theta = 0.0;
    double vega = 0.0;
};

struct StrategyHolding {
    UnderlyingPositionData underlyingPosition;
    std::unordered_map<std::string, OptionPositionData> optionPositions;
    std::unordered_map<std::string, ComboPositionData> comboPositions;
    PortfolioSummary summary;
};

}  // namespace utilities
