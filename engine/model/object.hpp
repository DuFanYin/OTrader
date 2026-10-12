#pragma once

/** Data structures (from object.py). */

#include "constant.hpp"
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace model {

using DateTime = std::chrono::system_clock::time_point;

/** Portfolio snapshot (option_apply_order). */
struct PortfolioSnapshot {
    std::string portfolio_name;
    DateTime datetime{};
    double underlying_bid = 0.0;
    double underlying_ask = 0.0;
    double underlying_mark = 0.0;
    /** The portfolio's hedge instrument, when it is not the underlying (else 0). */
    double hedge_bid = 0.0;
    double hedge_ask = 0.0;
    double hedge_mark = 0.0;
    /** Option values (option_apply_order): the quotes and their mark (model::mark), then what
     * pricing makes of them (Greeks per one option, not times its size). */
    std::vector<double> bid;
    std::vector<double> ask;
    std::vector<double> mark;
    std::vector<double> delta;
    std::vector<double> gamma;
    std::vector<double> theta;
    std::vector<double> vega;
    std::vector<double> iv;
    /** PortfolioData::price_snapshot's: each chain's forward (in the portfolio's pricing order),
     * each expiry's time to it in years (what the next pricing compares against), and whether it
     * has priced this snapshot's quotes. */
    std::vector<double> forward;
    std::vector<double> expiry_years;
    bool priced = false;
};

// Contract

struct ContractData {
    std::string symbol;
    std::string name;
    Product product = Product::UNKNOWN;
    double size = 1.0;
    double pricetick = 0.01;

    double min_volume = 1;
    std::optional<int> con_id;
    std::string trading_class; // empty: none

    std::optional<double> option_strike;
    std::optional<std::string> option_underlying;
    std::optional<OptionType> option_type;
    std::optional<DateTime> option_expiry;
};

// Order / Trade

struct Leg {
    int con_id = 0;
    int ratio = 0;
    Direction direction = Direction::LONG;
    std::optional<double> price;
    std::string symbol;
    std::string trading_class; // empty: none
};

/** A fill. datetime is {} when the venue did not say when. */
struct TradeData {
    std::string symbol;
    std::string orderid;
    std::string tradeid;
    Direction direction = Direction::LONG;
    double price = 0;
    double volume = 0;
    DateTime datetime{};
};

/** An order as the venue last reported it. datetime is {} when not known; a combo
 * (is_combo) has its legs and its combo_type, a single contract neither. */
struct OrderData {
    std::string symbol;
    std::string orderid;
    std::string trading_class; // empty: none

    OrderType type = OrderType::LIMIT;
    Direction direction = Direction::LONG;
    double price = 0;
    double volume = 0;
    double traded = 0;
    Status status = Status::SUBMITTING;
    DateTime datetime{};
    std::string reference;

    bool is_combo = false;
    std::vector<Leg> legs;
    ComboType combo_type = ComboType::CUSTOM;

    [[nodiscard]] bool is_active() const;
    struct CancelRequest create_cancel_request() const;
};

struct OrderRequest {
    std::string symbol;
    Direction direction = Direction::LONG;
    OrderType type = OrderType::LIMIT;
    double volume = 0;
    double price = 0;
    std::string reference;
    std::string trading_class; // empty: none
    bool is_combo = false;
    std::vector<Leg> legs;
    ComboType combo_type = ComboType::CUSTOM;

    OrderData create_order_data(const std::string& orderid) const;
};

struct CancelRequest {
    std::string orderid;
    std::string symbol;
    bool is_combo = false;
    std::vector<Leg> legs;
};

// Position Holding

struct BasePosition {
    std::string symbol;
    int quantity = 0;
    double avg_cost = 0.0;
    /// avg_cost * quantity * multiplier, signed like current_value(): negative for a short
    /// (premium received), so unrealized PnL = current_value() - cost_value for both sides.
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

/** An option position, or a combo position made of its legs. A combo's quantity is computed from
 * its legs (combo_quantity). */
struct OptionPositionData : BasePosition {
    std::optional<ComboType> combo_type;
    std::vector<OptionPositionData> legs;
    /** For a combo leg: contracts per combo unit, signed (bought +, sold -), with the first leg (by
     * symbol) positive; 0 otherwise. */
    int ratio = 0;

    OptionPositionData() { multiplier = 100.0; }
    OptionPositionData(const std::string& sym) : BasePosition{sym} { multiplier = 100.0; }
    void clear_fields();
    /** A combo's quantity: the first non-zero leg's quantity divided by its ratio (0 if none). */
    [[nodiscard]] int combo_quantity() const;
};

/** Shares: of the underlying, or of the hedge instrument when the underlying does not trade.
 * symbol is what they are in, named by the first fill (empty before). */
struct UnderlyingPositionData : BasePosition {
    UnderlyingPositionData() { delta = 1.0; }
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
    // The change in pnl since the engine first updated the holding, by source, summed over its
    // updates. Between two updates, what the holding at the first was worth more at the second:
    // its Greeks then times the underlying's move (delta, gamma), the days passed (theta) and each
    // option's implied vol change (vega); the rest of that is pnl_other. The rest of the change in
    // pnl (fills against the mid, settlements) is pnl_fills. In a backtest the first update finds
    // nothing held, so together they make pnl; live they start again at each start of the engine.
    double pnl_delta = 0.0;
    double pnl_gamma = 0.0;
    double pnl_theta = 0.0;
    double pnl_vega = 0.0;
    double pnl_other = 0.0;
    double pnl_fills = 0.0;
};

struct StrategyHolding {
    UnderlyingPositionData underlyingPosition;
    std::unordered_map<std::string, OptionPositionData> optionPositions;
    PortfolioSummary summary;

    /** f(position) for each option held: single options, and combos' legs (a combo holds nothing
     * beyond its legs). */
    template <typename F> void for_each_open_option(F&& f) const {
        for (const auto& [key, position] : optionPositions) {
            if (position.legs.empty()) {
                if (position.quantity != 0) {
                    f(position);
                }
                continue;
            }
            for (const OptionPositionData& leg : position.legs) {
                if (leg.quantity != 0) {
                    f(leg);
                }
            }
        }
    }

    /** Nothing open: no shares, no options, no combo or leg of one. */
    [[nodiscard]] bool is_flat() const {
        if (underlyingPosition.quantity != 0) {
            return false;
        }
        for (const auto& [key, position] : optionPositions) {
            if (position.quantity != 0) {
                return false;
            }
            for (const OptionPositionData& leg : position.legs) {
                if (leg.quantity != 0) {
                    return false;
                }
            }
        }
        return true;
    }
};

} // namespace model
