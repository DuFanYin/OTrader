#pragma once

/** Portfolio: its listing, its frame now, and the reads Option, Chain and Underlying. */

#include "model/constant.hpp"
#include "model/object.hpp"
#include "model/symbol.hpp"
#include "model/utility.hpp"
#include <chrono>
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

/** What IV is solved from, as price_snapshot takes it: "mid", "bid" or "ask" (any case), a view
 * that outlives any text; empty for anything else. */
std::string_view iv_price_mode_of(std::string_view text);

/** Each expiry's rate (rates at its time), each chain's forward by put-call parity, then each
 * option's IV and Greeks as Black on them. Any thread. With quotes_changed (one per option, on a snapshot priced before) only what moved is
 * priced again; a move of the underlying alone rescales delta and gamma. time_step (live): an
 * expiry's time counts as moved once per step while more than an hour is left. */
void price_snapshot(PortfolioSnapshot& snapshot, const RateCurve& rates,
                    std::string_view iv_price_mode,
                    std::span<const std::uint8_t> quotes_changed = {},
                    bool underlying_changed = true,
                    std::chrono::seconds time_step = std::chrono::seconds::zero());
/** Option `column`'s Greeks (per option): the frame's own when it holds them, else priced from
 * what its pricing keeps (the IV, the chain's forward, the expiry's time and rate, the spot, the
 * setting),
 * bit for bit what price_snapshot gives; zeros when the frame is not priced. */
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
    /** The ATM call's IV, else the put's. */
    [[nodiscard]] std::optional<double> atm_iv() const;
    /** The 25 delta risk reversal: the IV of the call at delta 0.25 less the put's at -0.25. */
    [[nodiscard]] std::optional<double> skew_25d() const;
    /** The far chain's ATM IV less this one's. */
    [[nodiscard]] std::optional<double> term_slope(const Chain& far) const;

  private:
    [[nodiscard]] const Listing::Chain& listed() const;

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
    std::string underlying_symbol;
    /** When the underlying does not trade (an index); empty: the underlying. */
    std::string hedge_symbol;
    /** Settles in shares (SPY), else in cash (SPXW). */
    bool physical = false;

    explicit Portfolio(std::string name);
    Portfolio(const Portfolio&) = delete;
    Portfolio& operator=(const Portfolio&) = delete;

    /** One rate at every maturity (ignored when not finite); or a curve. */
    void set_risk_free_rate(double rate);
    void set_rates(RateCurve rates);
    [[nodiscard]] const RateCurve& rates() const { return rates_; }
    /** mid, bid or ask: what IV is solved from; anything else is ignored. */
    void set_iv_price_mode(std::string_view mode);
    [[nodiscard]] std::string_view iv_price_mode() const { return iv_price_mode_; }

    // ---- Contracts

    void set_underlying(const Contract& contract);
    void set_hedge_instrument(const Contract& contract);
    /** The day's options, keeping the earlier ones `keep` names. Returns the chains dropped. */
    std::vector<std::string> replace_options(std::span<const Contract> options,
                                             const std::unordered_set<std::string>& keep);
    /** The values of what stays carry over to the next frame. Returns the chains dropped. */
    std::vector<std::string> list(std::shared_ptr<const Listing> listing);
    [[nodiscard]] const Listing& listing() const { return *listing_; }
    [[nodiscard]] const std::shared_ptr<const Listing>& shared_listing() const { return listing_; }
    /** A UTC midnight; each frame moves it to its own date. */
    void set_trading_day(DateTime ref) { dte_ref_ = ref; }
    [[nodiscard]] DateTime trading_day() const { return dte_ref_; }

    // ---- The quotes

    /** The last frame's time (the epoch before any). */
    [[nodiscard]] DateTime datetime() const { return datetime_; }
    [[nodiscard]] const Frame& frame() const { return frame_; }
    /** Zeros for its listing, to fill. */
    [[nodiscard]] PortfolioSnapshot snapshot() const;
    /** Takes the snapshot's values (pricing them if not priced) and leaves it the old ones. */
    void apply(PortfolioSnapshot& snapshot);
    /** Read in place while `storage` keeps them, else copied; priced if not. Of another listing:
     * only the time and the underlying's and hedge's quotes. */
    void apply(const Frame& frame, std::shared_ptr<const void> storage = {});
    void price_snapshot(PortfolioSnapshot& snapshot,
                        std::span<const std::uint8_t> quotes_changed = {},
                        bool underlying_changed = true,
                        std::chrono::seconds time_step = std::chrono::seconds::zero()) const;
    /** Live pricing's time_step, and the time left under which every step counts. */
    static constexpr std::chrono::seconds kLiveTimeStep{10};
    static constexpr std::chrono::hours kTimeStepMinTimeLeft{1};

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

    std::shared_ptr<const Listing> listing_;
    std::optional<Contract> underlying_;
    std::optional<Contract> hedge_instrument_;
    RateCurve rates_;
    std::string_view iv_price_mode_ = "mid"; // one of iv_price_mode_of's
    DateTime datetime_{};
    DateTime dte_ref_{};
    PortfolioSnapshot own_;
    Frame frame_; // own_, or values storage_ keeps
    std::shared_ptr<const void> storage_;
};

} // namespace model
