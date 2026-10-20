#pragma once

/** The engine's data: contracts, quotes, orders and fills, positions and holdings. */

#include "model/constant.hpp"
#include "model/parameters.hpp"
#include "model/symbol.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace model {

using DateTime = std::chrono::system_clock::time_point;

class Listing;
struct Order;

// ---- Contracts

struct Contract {
    /** A US equity or index option's, what an option is taken at when its contract is not known. */
    static constexpr double kOptionMultiplierDefault = 100.0;

    std::string symbol;
    std::string name;
    Product product = Product::UNKNOWN;
    double multiplier = 1.0;
    double price_tick = 0.01;

    double min_quantity = 1;
    std::optional<int> con_id;
    std::string trading_class; // empty: none

    std::optional<double> option_strike;
    std::optional<std::string> option_underlying;
    std::optional<OptionType> option_type;
    std::optional<DateTime> option_expiry;
};

// ---- Quotes

/** A portfolio's quotes at one time and their pricing, as views: of a PortfolioSnapshot's values
 * or of a stored day's. Per option columns follow its listing. */
struct Frame {
    const Listing* listing = nullptr;
    std::string_view portfolio_name;
    DateTime datetime{};
    double underlying_bid = 0.0;
    double underlying_ask = 0.0;
    double underlying_mark = 0.0;
    double hedge_bid = 0.0; // the hedge instrument's, when not the underlying (else 0)
    double hedge_ask = 0.0;
    double hedge_mark = 0.0;
    std::span<const double> bid;
    std::span<const double> ask;
    std::span<const double> bid_size; // contracts shown; 0: none known
    std::span<const double> ask_size;
    // Pricing (price_snapshot): Greeks per option, not times its size (see iv_price_mode); a
    // forward per chain, a time in years and a rate per expiry.
    std::span<const double> iv;
    std::span<const double> delta;
    std::span<const double> gamma;
    std::span<const double> theta;
    std::span<const double> vega;
    std::span<const double> forward;
    std::span<const double> expiry_years;
    std::span<const double> expiry_rate; // per expiry: the risk-free rate to it it was priced with
    // Per chain, kSmileValues each (pricing::fit_smiles): its smile as last fitted, by SmileValue.
    std::span<const double> smile;
    bool priced = false;
    double greeks_spot = 0.0; // the spot the Greeks stand at
    // Empty: it holds its Greeks (delta..vega). Else the setting it was priced with, its Greeks
    // priced from its IVs with it when asked (greeks_at).
    std::string_view iv_price_mode;
};

/** Where each of a chain's values is in Frame::smile: raw SVI (quant::Svi), the forward and years
 * it was fitted at (the years also when a fit found too few quotes), its error and points, 1 when
 * it is free of butterfly arbitrage (quant::SviFit); all 0 before a fit. */
enum SmileValue : std::uint8_t {
    kSviA,
    kSviB,
    kSviRho,
    kSviM,
    kSviSigma,
    kSmileForward,
    kSmileYears,
    kSmileError,
    kSmilePoints,
    kSmileButterflyFree,
    kSmileValues // a chain's
};

class DailyTable;

/** A risk-free rate by time to maturity, continuously compounded, a year: a straight line between
 * its points (by years, increasing), flat before the first and after the last. One point: that
 * rate at every maturity. */
struct RateCurve {
    std::vector<double> years{0.0};
    std::vector<double> rates{0.05};

    /** The rate to a maturity `t` years away. */
    [[nodiscard]] double at(double t) const;
    /** One rate at every maturity. */
    [[nodiscard]] static RateCurve flat(double rate);
    /** The Treasury's constant-maturity yields in `daily` last dated before `day` (DGS1MO,
     * DGS3MO, DGS6MO, DGS1, DGS2: percent, compounded twice a year) made continuously
     * compounded, as the backtest's days carry theirs; none unless each has a value. */
    [[nodiscard]] static std::optional<RateCurve> treasury(const DailyTable& daily,
                                                           std::chrono::year_month_day day);
    bool operator==(const RateCurve&) const = default;
};

/** An option's Greeks: per option, or times its size where said. */
struct Greeks {
    double delta = 0.0;
    double gamma = 0.0;
    double theta = 0.0;
    double vega = 0.0;
};

/** A frame that owns its values: what live market data fills and the event loop carries. */
class PortfolioSnapshot : public Frame {
  public:
    /** Its per option columns, in the order its values hold them. */
    static constexpr std::array kOptionColumns{
        &Frame::bid,   &Frame::ask,   &Frame::bid_size, &Frame::ask_size, &Frame::iv,
        &Frame::delta, &Frame::gamma, &Frame::theta,    &Frame::vega};

    PortfolioSnapshot() = default;
    /** Zeros in every column of the listing. */
    PortfolioSnapshot(std::shared_ptr<const Listing> listing, std::string portfolio_name);
    PortfolioSnapshot(const PortfolioSnapshot& other);
    PortfolioSnapshot(PortfolioSnapshot&& other) noexcept;
    PortfolioSnapshot& operator=(const PortfolioSnapshot& other);
    PortfolioSnapshot& operator=(PortfolioSnapshot&& other) noexcept;
    ~PortfolioSnapshot() = default;

    [[nodiscard]] const std::shared_ptr<const Listing>& shared_listing() const { return owner_; }
    /** A column to write, named as &Frame::bid. */
    [[nodiscard]] std::span<double> values(std::span<const double> Frame::* column);
    /** The frame's values, which must be of this listing; a column it lacks is zeros. */
    void assign(const Frame& frame);
    /** Laid out for another listing in the buffer it has, all zeros, its time and quotes cleared:
     * the values of the chains `written` names (indices into its listing now) and of every expiry
     * are zeroed, the others being zeros already, as in a snapshot only those chains were written
     * into since it was made (a backtest's replay, a day after the other: adapters::Replay). */
    void relist(std::shared_ptr<const Listing> listing, std::span<const std::uint32_t> written);

    /** Live: when its first quote arrived (utilities::tsc_now); 0 in a backtest. */
    std::uint64_t quoted_tsc = 0;

  private:
    void point(); // the views at values_

    std::shared_ptr<const Listing> owner_;
    std::string name_;
    std::vector<double> values_; // the columns one after another
};

// ---- Orders and fills

struct Leg {
    int con_id = 0;
    int ratio = 0;
    Side side = Side::BUY;
    std::optional<double> price;
    std::string symbol;
    std::string trading_class; // empty: none
};

struct CancelRequest {
    std::string order_id;
    std::string symbol;
};

/** What a strategy or the engine asks a venue to trade: units of its legs (a contract's own order
 * is one leg at ratio 1). Built by model::order_request. */
struct OrderRequest {
    std::string symbol; // the contract's, or <combo type>_<its legs' symbols>
    OrderType type = OrderType::LIMIT;
    double quantity = 0;
    double price = 0; // net per unit: paid positive, received negative; 0 at market
    std::string reference;
    std::string trading_class; // empty: none
    std::vector<Leg> legs;
    ComboType combo_type = ComboType::SINGLE_LEG;

    /** More than one leg, or one at a ratio other than 1. */
    [[nodiscard]] bool is_combo() const;
    Order create_order(const std::string& order_id) const;
};

/** An order as the venue last reported it, in units of its legs. datetime {}: not known. */
struct Order {
    std::string symbol; // as OrderRequest's
    std::string order_id;
    std::string trading_class; // empty: none

    OrderType type = OrderType::LIMIT;
    double price = 0; // as OrderRequest's
    double quantity = 0;
    double filled = 0;
    Status status = Status::SUBMITTING;
    DateTime datetime{};
    std::string reference;

    std::vector<Leg> legs;
    ComboType combo_type = ComboType::SINGLE_LEG;

    [[nodiscard]] bool is_active() const;
    /** More than one leg, or one at a ratio other than 1. */
    [[nodiscard]] bool is_combo() const;
    CancelRequest create_cancel_request() const;
};

/** A fill. datetime {}: the venue did not say when. */
struct Fill {
    std::string symbol;
    std::string order_id;
    std::string fill_id;
    Side side = Side::BUY;
    double price = 0;
    double quantity = 0;
    DateTime datetime{};
};

/** A MARKET order of one contract filled at once at `price`, and its fill (fill_id: the
 * order_id). */
std::pair<Order, Fill> filled_order(const std::string& order_id, const std::string& symbol,
                                    Side side, double quantity, double price,
                                    DateTime at, std::string reference);

// ---- Positions and holdings

struct Position {
    std::string symbol;
    int quantity = 0;
    double avg_cost = 0.0;
    /** avg_cost * quantity * multiplier, signed as current_value() (a short's negative), so
     * unrealized P&L is current_value() - cost_value on both sides. */
    double cost_value = 0.0;
    double realized_pnl = 0.0;
    double mid_price = 0.0;
    double delta = 0.0;
    double gamma = 0.0;
    double theta = 0.0;
    double vega = 0.0;
    double multiplier = 1.0;

    [[nodiscard]] double current_value() const;
    /** Flat: everything but its realized P&L back to zero. */
    void clear_fields();
};

/** One option held, in contracts. Booked by contract; `combo` only says what it was opened in. */
struct OptionPosition : Position {
    /** The order that opened it from flat: its legs' symbols sorted and joined by '|' (its own
     * symbol alone), and its type. What it is shown and closed with. */
    std::string combo;
    ComboType combo_type = ComboType::SINGLE_LEG;
    /** Its symbol read once; of a symbol no option's, none and `expires` never. */
    OptionId id;
    DateTime expires = DateTime::max();

    OptionPosition(const std::string& sym, double contract_multiplier);
};

/** Shares of the underlying, or of the hedge instrument when the underlying does not trade;
 * symbol: the first fill's (empty before). */
struct SharePosition : Position {
    SharePosition() { delta = 1.0; }
};

struct HoldingSummary {
    double total_cost = 0.0;
    double current_value = 0.0;
    double unrealized_pnl = 0.0;
    double realized_pnl = 0.0;
    double pnl = 0.0;
    double delta = 0.0;
    double gamma = 0.0;
    double theta = 0.0;
    double vega = 0.0;
    // The change in pnl since the engine first updated the holding, by source. Between updates the
    // holding then is valued at its Greeks times the underlying's move (delta, gamma), the days
    // passed (theta) and each option's IV change (vega); the rest of that move is pnl_other, the
    // rest of the change (fills against the mid, settlements) pnl_fills. A backtest starts flat,
    // so they sum to pnl; live they start again with each engine.
    double pnl_delta = 0.0;
    double pnl_gamma = 0.0;
    double pnl_theta = 0.0;
    double pnl_vega = 0.0;
    double pnl_other = 0.0;
    double pnl_fills = 0.0;
};

struct StrategyHolding {
    SharePosition shares;
    /** By symbol, the options open. */
    std::unordered_map<std::string, OptionPosition> options;
    /** Closed ones with their realized P&L, apart so that marking does not grow with every
     * contract ever traded; one traded again moves back. */
    std::unordered_map<std::string, OptionPosition> closed_options;
    double closed_realized_pnl = 0.0; // closed_options', each to the cent
    /** Its latest order's time as sent, and its latest fill's (a settlement's too): rebooked on
     * a restart, unlike a strategy's own members. */
    DateTime last_order{};
    DateTime last_fill{};
    HoldingSummary summary;
    /** Its summary computed since the engine started (BookkeepingEngine::value_holding). */
    bool summarized = false;

    template <typename F> void for_each_open_option(F&& f) const {
        for (const auto& [symbol, position] : options) {
            if (position.quantity != 0) {
                f(position);
            }
        }
    }

    /** The open options `accept` takes, by the combo each was opened in, each group by symbol. */
    template <typename F>
    [[nodiscard]] std::map<std::string, std::vector<OptionPosition>>
    open_options_by_combo(F&& accept) const {
        std::map<std::string, std::vector<OptionPosition>> out;
        for_each_open_option([&](const OptionPosition& p) {
            if (accept(p)) {
                out[p.combo.empty() ? p.symbol : p.combo].push_back(p);
            }
        });
        for (auto& [_, options] : out) {
            std::ranges::sort(options, {}, &OptionPosition::symbol);
        }
        return out;
    }

    /**
     * USD its open options can lose at most, from what they cost: on one expiry, that cost less
     * the worst of their value at expiry (at zero, at each strike, and past the last when the calls
     * bought cover those sold); on several, or with a loss that has no bound, what they cost. NaN
     * when there is no bound to give (no options, or a credit taken with a loss unbounded).
     */
    [[nodiscard]] double max_loss() const;

    /** No shares, no options. */
    [[nodiscard]] bool is_flat() const {
        bool open = shares.quantity != 0;
        for_each_open_option([&open](const OptionPosition&) { open = true; });
        return !open;
    }
    /** Its summary values all of it: valued since the engine started, every open position at a
     * mark (quoted once since then). */
    [[nodiscard]] bool valued() const {
        bool marked = summarized && (shares.quantity == 0 || shares.mid_price > 0.0);
        for_each_open_option([&marked](const OptionPosition& p) {
            marked = marked && p.mid_price > 0.0;
        });
        return marked;
    }
};

/** A strategy as it is hosted and kept across restarts: enough to make it again as it was. Its
 * setting is resolved: every parameter its class declares. */
struct StrategyRecord {
    std::string name;
    std::string class_name;
    std::string portfolio;
    Values setting;
    StrategyState state = StrategyState::Added;
};

} // namespace model
