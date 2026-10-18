#pragma once

// Option strategy screener: chain input, filters, strategy generators and ranking.

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace quant {

// ── Chain input ───────────────────────────────────────────────────────────────

// One listed option. Prices are in quote currency (USD) per 1 unit of underlying;
// the contract multiplier is applied when strategy metrics are computed.
struct ChainOption {
    std::string symbol;
    std::string expiry;           // opaque label, grouped on; ordering uses `years`
    double strike   = 0.0;
    bool   is_call  = true;
    double forward  = 0.0;        // forward / futures price for this expiry
    double years    = 0.0;        // time to expiry in years
    double bid      = std::numeric_limits<double>::quiet_NaN();
    double ask      = std::numeric_limits<double>::quiet_NaN();
    double mark     = 0.0;        // fallback price when bid/ask is missing
    double iv       = 0.0;        // decimal; <= 0 means unknown
    double volume   = 0.0;
    double oi       = 0.0;
    // Greeks per 1 unit of underlying: delta, gamma, vega per 1 vol point, theta per day.
    // Filled by screen_compute_greeks() unless supplied by the caller.
    double delta = 0.0, gamma = 0.0, theta = 0.0, vega = 0.0;
};

// ── Filters / settings ────────────────────────────────────────────────────────

struct ScreenRange {
    bool   on = false;
    double lo = 0.0;
    double hi = 0.0;
    // Inclusive. NaN never passes an active range; an inactive range passes everything.
    bool admits(double v) const { return !on || (!std::isnan(v) && v >= lo && v <= hi); }
};

struct ScreenOptionFilter {
    std::optional<double>      min_volume;
    std::optional<double>      min_oi;
    std::optional<double>      min_price;               // on mid price
    std::optional<std::string> expiry;
    ScreenRange                days_to_expiry;          // years * 365
    ScreenRange                volume_ratio;            // volume / oi
    std::optional<double>      max_bid_ask_spread;      // absolute, quote currency
    std::optional<double>      max_bid_ask_spread_pct;  // (ask - bid) / mid
    ScreenRange                moneyness;               // strike / forward
    bool                       require_two_sided = true;
};

struct ScreenStrategyFilter {
    bool long_direction = true;
    ScreenRange debit, credit, gain, loss, rr;
    ScreenRange net_delta, net_theta, net_vega, iv;
    ScreenRange forward_vol, edge;
};

struct ScreenStrategyToggles {
    bool single_calls = false;
    bool iron_condors = false;
    bool straddles    = false;
    bool strangles    = false;
    bool forward_vols = false;
};

enum class ScreenPriceMode { Executable = 0, Mid = 1 };   // Executable: buy @ ask, sell @ bid
enum class ScreenRankKey   { RR, Gain, Loss, Cost, Credit, Edge, ForwardVol };

struct ScreenRank {
    ScreenRankKey key        = ScreenRankKey::RR;
    bool          descending = true;
    int           top_n      = 20;
};

// ── Strategy records ──────────────────────────────────────────────────────────

enum class ScreenKind : uint8_t { Single, IronCondor, Straddle, Strangle, ForwardVol };

struct ScreenLeg {
    int option_index = -1;   // index into the chain
    int qty          = 0;    // +1 buy, -1 sell
};

struct ScreenMetrics {
    double debit = 0.0, credit = 0.0, cost = 0.0;
    double max_gain = 0.0, max_loss = 0.0, rr = 0.0;
    double net_delta = 0.0, net_gamma = 0.0, net_theta = 0.0, net_vega = 0.0;
    double avg_iv      = std::numeric_limits<double>::quiet_NaN();
    double model_value = std::numeric_limits<double>::quiet_NaN();
    double edge        = std::numeric_limits<double>::quiet_NaN();   // model_value - cost
    double forward_vol = std::numeric_limits<double>::quiet_NaN();
};

struct StrategyRecord {
    ScreenKind               kind = ScreenKind::Single;
    bool                     long_direction = true;
    int                      n_legs = 0;
    std::array<ScreenLeg, 4> legs{};
    ScreenMetrics            m;
    uint64_t                 seq = 0;   // generation order; total-order tie-break for ranking
};

// Everything a generator needs, shared read-only across threads.
struct ScreenContext {
    std::span<const ChainOption> chain;
    std::span<const double>      model_px;   // per chain index, per 1 unit; NaN if unavailable
    double                       multiplier = 1.0;
    ScreenPriceMode              price_mode = ScreenPriceMode::Executable;
    const ScreenStrategyFilter*  filter = nullptr;
    ScreenRank                   rank;
};

// Bounded best-N buffer: memory stays O(top_n) however many combinations are generated.
class ScreenTopN {
public:
    explicit ScreenTopN(ScreenRank rank);
    void push(const StrategyRecord& s);
    void merge(ScreenTopN&& other);
    std::vector<StrategyRecord> finish();   // sorted best-first, at most top_n
    bool better(const StrategyRecord& a, const StrategyRecord& b) const;
private:
    void shrink();
    ScreenRank rank_;
    std::vector<StrategyRecord> buf_;
};

struct ScreenCounts {
    long long generated = 0;   // combinations constructed
    long long passed    = 0;   // survived strategy-level filters
};

// ── Black-76 (forward-based) pricing ──────────────────────────────────────────

double black76_price (double forward, double strike, double rate, double vol, double years, bool is_call);
void   black76_greeks(double forward, double strike, double rate, double vol, double years, bool is_call,
                      double* out_delta, double* out_gamma, double* out_theta_day, double* out_vega_pt);

// ── Option-level primitives ───────────────────────────────────────────────────

double screen_mid(const ChainOption& o);                                   // else mark
double screen_fill_price(const ChainOption& o, int qty, ScreenPriceMode mode);  // NaN if not fillable

std::vector<int> screen_filter_options(std::span<const ChainOption> chain, const ScreenOptionFilter& f);
void             screen_compute_greeks(std::span<ChainOption> chain, std::span<const int> indices, double rate);

// ── Strategy generators ───────────────────────────────────────────────────────
// Each generator enumerates combinations of the filtered options, evaluates
// metrics, applies the strategy-level filter and keeps the best `rank.top_n`.

void screen_generate_single_calls(const ScreenContext& c, std::span<const int> idx, ScreenTopN& out, ScreenCounts& n);
void screen_generate_iron_condors(const ScreenContext& c, std::span<const int> idx, ScreenTopN& out, ScreenCounts& n);
void screen_generate_straddles   (const ScreenContext& c, std::span<const int> idx, ScreenTopN& out, ScreenCounts& n);
void screen_generate_strangles   (const ScreenContext& c, std::span<const int> idx, ScreenTopN& out, ScreenCounts& n);
void screen_generate_forward_vols(const ScreenContext& c, std::span<const int> idx, ScreenTopN& out, ScreenCounts& n);

// Fills s.m from the legs. Returns false if any leg cannot be priced.
bool screen_evaluate(const ScreenContext& c, StrategyRecord& s);
bool screen_pass_strategy_filter(const ScreenStrategyFilter& f, const ScreenMetrics& m);

}  // namespace quant
