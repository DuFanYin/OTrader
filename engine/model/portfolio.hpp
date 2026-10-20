#pragma once

/** Portfolio: its listing, its frame now, and the reads Option, Chain and Underlying. */

#include "model/constant.hpp"
#include "model/object.hpp"
#include "model/portfolio_spec.hpp"
#include "model/symbol.hpp"
#include "model/utility.hpp"
#include "quant/engine_api.h"
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace model {

/** The options a portfolio lists, in a frame's column order (chains by symbol, then options by
 * symbol). Immutable: a roll builds another. */
class Listing {
  public:
    struct Chain {
        std::string symbol;
        DateTime expiry{};
        std::uint32_t expiry_index = 0; // into expiries()
        std::uint32_t first = 0;        // its columns: [first, first + count)
        std::uint32_t count = 0;
        std::vector<double> strikes; // ascending; per strike the call's and put's column (-1: none)
        std::vector<std::int32_t> calls;
        std::vector<std::int32_t> puts;
    };

    Listing() = default;
    /** The options among the contracts, taken; of one given twice, the last. Given in this order
     * (as a stored day keeps them), one pass; else sorted first. */
    explicit Listing(std::vector<Contract> contracts);
    Listing(const Listing&) = delete; // shared, never copied
    Listing& operator=(const Listing&) = delete;

    [[nodiscard]] std::size_t size() const { return contracts_.size(); }
    [[nodiscard]] std::span<const Contract> contracts() const { return contracts_; }
    [[nodiscard]] const Contract& contract(std::size_t column) const {
        return contracts_[column];
    }
    [[nodiscard]] double strike(std::size_t column) const { return strikes_[column]; }
    [[nodiscard]] OptionType type(std::size_t column) const { return types_[column]; }
    [[nodiscard]] std::uint32_t chain_of(std::size_t column) const { return chain_of_[column]; }
    [[nodiscard]] std::optional<std::uint32_t> column(std::string_view symbol) const;
    [[nodiscard]] std::span<const Chain> chains() const { return chains_; }
    [[nodiscard]] std::optional<std::uint32_t> chain(std::string_view symbol) const;
    /** Each once, in the chains' order. */
    [[nodiscard]] std::span<const DateTime> expiries() const { return expiries_; }

  private:
    std::vector<Contract> contracts_;
    std::vector<double> strikes_;
    std::vector<OptionType> types_;
    std::vector<std::uint32_t> chain_of_;
    std::vector<Chain> chains_; // by symbol: chain() searches them
    std::vector<DateTime> expiries_;
    // By symbol's hash, probed in turn: a column + 1, 0 for none; twice as many as columns, a
    // power of two.
    std::vector<std::uint32_t> columns_;
};

/** What IV is solved from, as pricing::price_snapshot takes it: "mid", "bid" or "ask" (any case),
 * a view that outlives any text; empty for anything else. */
std::string_view iv_price_mode_of(std::string_view text);

// ---- How a frame's options are priced: pricing::price_snapshot prices them so, and greeks_at one
// again from what a frame keeps.

enum class IvInput : std::uint8_t { Mid, Bid, Ask };

/** Of iv_price_mode_of's modes; mid for any other. */
inline IvInput iv_input(std::string_view mode) {
    if (mode == "bid") {
        return IvInput::Bid;
    }
    return mode == "ask" ? IvInput::Ask : IvInput::Mid;
}

/** The price IV is solved from; 0 for none. */
inline double iv_input_price(double bid, double ask, IvInput input) {
    if (bid > 0.0 && ask > 0.0 && bid > ask) {
        return 0.0; // crossed: no valid quote
    }
    switch (input) {
    case IvInput::Bid:
        return bid > 0.0 ? bid : 0.0;
    case IvInput::Ask:
        return ask > 0.0 ? ask : 0.0;
    case IvInput::Mid:
        break;
    }
    return mark(bid, ask);
}

/** An option with no spot, strike, time left or price to price it from: no IV, no Greeks. */
inline bool cannot_price(double spot, double strike, double years, double price) {
    return spot <= 0.0 || strike <= 0.0 || years <= 0.0 || price <= 0.0;
}

/** The forward an option is priced on: its chain's by parity, else the spot grown at the rate. */
inline double pricing_forward(double chain_forward, double spot, double rate, double years) {
    return chain_forward <= 0.0 ? spot * std::exp(rate * years) : chain_forward;
}

/** The underlying's mark from its quotes, else as marked. */
inline double frame_spot(const Frame& frame) {
    return mark(frame.underlying_bid, frame.underlying_ask, frame.underlying_mark);
}

/** Option `column`'s Greeks (per option): the frame's own when it holds them, else priced from
 * what its pricing keeps (the IV, the chain's forward, the expiry's time and rate, the spot, the
 * setting), bit for bit what pricing::price_snapshot gives; zeros when the frame is not priced. */
Greeks greeks_at(const Frame& frame, std::size_t column);

class Portfolio;
class Option;

/** A chain of a portfolio, read now. Valid until its next roll. */
class Chain {
  public:
    /** A parity forward this far from spot is bad quotes: grown spot instead. */
    static constexpr double kMaxForwardGap = 0.1;

    Chain(const Portfolio& portfolio, std::uint32_t index);

    [[nodiscard]] std::uint32_t index() const { return index_; }
    [[nodiscard]] const std::string& symbol() const { return listed().symbol; }
    [[nodiscard]] DateTime expiry() const { return listed().expiry; }
    [[nodiscard]] std::span<const double> strikes() const { return listed().strikes; }
    [[nodiscard]] std::optional<Option> call(double strike) const;
    [[nodiscard]] std::optional<Option> put(double strike) const;
    [[nodiscard]] std::optional<Option> option(OptionType type, double strike) const;
    /** Whole days from the portfolio's trading_day; 0 once there. */
    [[nodiscard]] int days_to_expiry() const;
    /** By put-call parity, else the spot grown at the rate; 0 before any pricing, and once
     * expired. */
    [[nodiscard]] double forward() const;
    /** The risk-free rate to its expiry it is priced with (continuously compounded, a year); 0
     * before any pricing. */
    [[nodiscard]] double rate() const;
    /** The forward, else the underlying's mid. */
    [[nodiscard]] double reference_price() const;
    /** Nearest reference_price(), the lower of two; unquoted: the middle one; none: 0. */
    [[nodiscard]] double atm_strike() const;
    /** Its smile as last fitted (pricing::fit_smiles); points 0 before one, or when too few of
     * its options were quoted. */
    [[nodiscard]] quant::SviFit smile() const;
    /** The IV on its smile at a strike, at the forward and years it was fitted at. */
    [[nodiscard]] std::optional<double> smile_iv(double strike) const;
    /** The IV on its smile where an option's delta (as Option::delta: a call's above 0, a put's
     * below) is `delta`; none where its smile does not reach it. */
    [[nodiscard]] std::optional<double> smile_iv_at_delta(double delta) const;
    /** The IV on its smile at the forward it was fitted at. */
    [[nodiscard]] std::optional<double> atm_iv() const;
    /** The 25 delta risk reversal on its smile: the IV at a call's delta 0.25 less a put's at
     * -0.25. */
    [[nodiscard]] std::optional<double> skew_25d() const;
    /** The far chain's ATM IV less this one's. */
    [[nodiscard]] std::optional<double> term_slope(const Chain& far) const;

  private:
    [[nodiscard]] const Listing::Chain& listed() const;
    /** Its kSmileValues in the frame; empty when not fitted. */
    [[nodiscard]] std::span<const double> fitted() const;

    const Portfolio* portfolio_;
    std::uint32_t index_;
};

/** An option of a portfolio, read now. Valid until its next roll. */
class Option {
  public:
    Option(const Portfolio& portfolio, std::uint32_t column);

    [[nodiscard]] std::uint32_t column() const { return column_; }
    [[nodiscard]] const std::string& symbol() const;
    [[nodiscard]] double strike() const;
    [[nodiscard]] OptionType type() const;
    [[nodiscard]] DateTime expiry() const;
    /** The contract's multiplier. */
    [[nodiscard]] double multiplier() const;
    [[nodiscard]] Chain chain() const;

    [[nodiscard]] double bid() const;
    [[nodiscard]] double ask() const;
    /** model::mark of its quotes. */
    [[nodiscard]] double mid() const;
    [[nodiscard]] double bid_size() const;
    [[nodiscard]] double ask_size() const;
    [[nodiscard]] double iv() const;
    /** Greeks of one contract (times size). */
    [[nodiscard]] Greeks greeks() const;
    [[nodiscard]] double delta() const;
    [[nodiscard]] double gamma() const;
    [[nodiscard]] double theta() const;
    [[nodiscard]] double vega() const;

    bool operator==(const Option& other) const = default;

  private:
    [[nodiscard]] double value(std::span<const double> Frame::* values) const;

    const Portfolio* portfolio_;
    std::uint32_t column_;
};

/** A portfolio's underlying or hedge instrument, read now. */
class Underlying {
  public:
    Underlying(const Portfolio& portfolio, bool hedge) : portfolio_(&portfolio), hedge_(hedge) {}

    [[nodiscard]] const std::string& symbol() const;
    [[nodiscard]] double multiplier() const;
    [[nodiscard]] double bid() const;
    [[nodiscard]] double ask() const;
    [[nodiscard]] double mid() const;
    /** Units of the underlying per unit of it at equal value; a hedge's is 0 until both are
     * priced. */
    [[nodiscard]] double theo_delta() const;

  private:
    [[nodiscard]] const Contract& contract() const;

    const Portfolio* portfolio_;
    bool hedge_;
};

/** What a portfolio trades and its frame now. A frame is read where it is (a stored day, a
 * snapshot taken over), not copied. */
class Portfolio {
  public:
    std::string name;

    /** spec: what it is (one of model::kPortfolios), outliving it; none: what set_underlying and
     * set_hedge_instrument give it, settled in cash. */
    explicit Portfolio(std::string name, const PortfolioSpec* spec = nullptr);
    Portfolio(const Portfolio&) = delete;
    Portfolio& operator=(const Portfolio&) = delete;

    [[nodiscard]] const PortfolioSpec* spec() const { return spec_; }
    /** Its spec's (SPX for SPXW), else its underlying's; empty before one is set. */
    [[nodiscard]] std::string_view underlying_symbol() const;
    /** What hedges trade when the underlying does not (an index); empty: the underlying. */
    [[nodiscard]] std::string_view hedge_symbol() const;
    /** Settles in shares (SPY), else in cash (SPXW). */
    [[nodiscard]] bool physical() const { return spec_ != nullptr && spec_->physical; }
    /** Its spec's, else NYSE's. */
    [[nodiscard]] const SessionCalendar& calendar() const {
        return spec_ != nullptr ? *spec_->calendar : kNyse;
    }

    // ---- Contracts

    void set_underlying(const Contract& contract);
    void set_hedge_instrument(const Contract& contract);
    /** The day's options, keeping the earlier ones `keep` names. Returns the chains dropped. */
    std::vector<std::string> replace_options(std::span<const Contract> options,
                                             const std::unordered_set<std::string>& keep);
    /** The values of what stays carry over to the next frame: of the chains `valued` names when
     * given (the frame holds none of the others' values), else of all. Returns the chains
     * dropped. */
    std::vector<std::string> list(std::shared_ptr<const Listing> listing,
                                  std::optional<std::span<const std::string>> valued = {});
    [[nodiscard]] const Listing& listing() const { return *listing_; }
    [[nodiscard]] const std::shared_ptr<const Listing>& shared_listing() const { return listing_; }
    /** A UTC midnight: of the date on its calendar; each frame moves it to its own. */
    void set_trading_day(DateTime ref) { dte_ref_ = ref; }
    [[nodiscard]] DateTime trading_day() const { return dte_ref_; }
    /** The underlying's mark at its first frame in today's session (from the open's first minute
     * to the close, on its calendar), and its highest and lowest since; 0 before one. */
    [[nodiscard]] double day_open() const { return day_open_; }
    [[nodiscard]] double day_high() const { return day_high_; }
    [[nodiscard]] double day_low() const { return day_low_; }
    /** The minute since the open of that first frame (1: the open's bar); -1 before one. */
    [[nodiscard]] int day_first_minute() const { return day_first_minute_; }
    /** Its first frame today was in the session's first five minutes (or restore_day said where
     * the day began). */
    [[nodiscard]] bool saw_open() const { return day_first_minute_ >= 1 && day_first_minute_ <= 5; }
    /** The underlying's open, high and low of `day` until now as the source has them, for an
     * engine that started after the open: from the open's bar on; another day's are left out. */
    void restore_day(std::chrono::year_month_day day, double open, double high, double low);

    // ---- The quotes

    /** The last frame's time (the epoch before any). */
    [[nodiscard]] DateTime datetime() const { return datetime_; }
    [[nodiscard]] const Frame& frame() const { return frame_; }
    /** Zeros for its listing, to fill. */
    [[nodiscard]] PortfolioSnapshot snapshot() const;
    /** Takes the snapshot's values, priced already (pricing::price_snapshot), and leaves it the
     * old ones. */
    void apply(PortfolioSnapshot& snapshot);
    /** Read in place while `storage` keeps them, else copied; priced already. Of another listing:
     * only the time and the underlying's and hedge's quotes. */
    void apply(const Frame& frame, std::shared_ptr<const void> storage = {});

    // ---- Reads

    [[nodiscard]] std::optional<Underlying> underlying() const;
    /** What hedges trade: hedge_symbol's, else the underlying; none until loaded. */
    [[nodiscard]] std::optional<Underlying> hedge() const;
    [[nodiscard]] std::optional<Underlying> instrument(std::string_view symbol) const;
    [[nodiscard]] std::optional<Option> option(std::string_view symbol) const;
    [[nodiscard]] std::optional<Chain> chain(std::string_view symbol) const;
    /** An option's, the underlying's or the hedge's. */
    [[nodiscard]] const Contract* contract(std::string_view symbol) const;
    /** By symbol. */
    [[nodiscard]] std::vector<std::string> chains_by_expiry(int min_dte, int max_dte) const;

  private:
    friend class Underlying;

    void read_own(); // frame_ at own_
    void move_time(DateTime time);

    const PortfolioSpec* spec_;
    std::shared_ptr<const Listing> listing_;
    std::optional<Contract> underlying_;
    std::optional<Contract> hedge_instrument_;
    DateTime datetime_{};
    DateTime dte_ref_{};
    DateTime day_of_open_{}; // the day day_open_.. are of
    double day_open_ = 0.0;
    double day_high_ = 0.0;
    double day_low_ = 0.0;
    int day_first_minute_ = -1;
    PortfolioSnapshot own_;
    // The chains whose values list() carried into own_, while nothing else has written it.
    std::optional<std::vector<std::uint32_t>> own_written_;
    Frame frame_; // own_, or values storage_ keeps
    std::shared_ptr<const void> storage_;
};

// ---- Legs: how a leg of a combo picks its option

/**
 * A leg as a rule: which chain (by days to expiry) and which strike. One rule picks the same option
 * in a strategy (ComboStrategy), on paper and in research (research/structures), at the same
 * quotes.
 */
struct LegRule {
    enum class StrikeBy : std::uint8_t {
        kForwardPct,   // strike: the chain's forward x (1 + strike / 100), the nearest listed
        kImpliedMoves, // forward x (1 + strike x the implied move: the ATM straddle's mid / forward)
        kDelta,        // the option whose delta (a call's above 0, a put's below) is nearest strike
    };
    enum class ExpiryBy : std::uint8_t {
        kFirstAtLeast, // the first expiry dte days out or more
        kExactly,      // dte days out, no other
        kNearest,      // the nearest dte days out within min_dte..max_dte (the earlier on a tie)
    };
    int ratio = 0; // contracts a unit: bought +, sold -; 0: no leg
    OptionType right = OptionType::CALL;
    StrikeBy strike_by = StrikeBy::kForwardPct;
    double strike = 0.0;
    ExpiryBy expiry_by = ExpiryBy::kFirstAtLeast;
    int dte = 0;
    int min_dte = 0;
    int max_dte = 0;
};

/** The chain a leg picks among those listed now (a symbol); empty when none fits. */
std::string leg_chain(const Portfolio& portfolio, const LegRule& rule);
/** The option a leg picks on its chain now; none when the chain is not priced or has no strikes,
 * by implied moves when its ATM straddle is not quoted both ways, by delta when none is priced. */
std::optional<Option> leg_option(const Chain& chain, const LegRule& rule);

} // namespace model
