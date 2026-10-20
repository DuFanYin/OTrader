#include "research/fills.hpp"

#include "quant/engine_api.h"

#include <arrow/api.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <random>
#include <stdexcept>

namespace research {

namespace {

constexpr double kEps = 1e-9;
constexpr double kInf = std::numeric_limits<double>::infinity();
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

/** The last index at or before t, -1 when none. */
auto at_or_before(std::span<const std::int64_t> ms, std::int64_t t) -> std::ptrdiff_t {
    return std::ranges::upper_bound(ms, t) - ms.begin() - 1;
}

auto after(std::span<const std::int64_t> ms, std::int64_t t) -> std::ptrdiff_t {
    return std::ranges::upper_bound(ms, t) - ms.begin();
}

auto two_sided(double bid, double ask) -> bool { return bid > 0.0 && ask > bid; }


} // namespace

auto simulate_limits(const QuoteSeries& quotes, const TradeSeries& trades,
                     const LimitOrders& orders) -> std::vector<LimitFill> {
    std::vector<LimitFill> out;
    const std::span<const std::int64_t> ms(quotes.ms);
    const std::span<const std::int64_t> tms(trades.ms);
    for (const std::int64_t t0 : orders.t0_ms) {
        const std::ptrdiff_t i0 = at_or_before(ms, t0);
        if (i0 < 0) {
            continue;
        }
        const double b0 = quotes.bid[static_cast<std::size_t>(i0)];
        const double a0 = quotes.ask[static_cast<std::size_t>(i0)];
        if (!two_sided(b0, a0)) {
            continue;
        }
        const double mid0 = (a0 + b0) / 2.0;
        const double half0 = (a0 - b0) / 2.0;
        for (const std::int64_t w : orders.window_ms) {
            const std::ptrdiff_t i1 = at_or_before(ms, t0 + w);
            const double b1 = quotes.bid[static_cast<std::size_t>(i1)];
            const double a1 = quotes.ask[static_cast<std::size_t>(i1)];
            if (!two_sided(b1, a1)) {
                continue;
            }
            const double mid1 = (a1 + b1) / 2.0;
            // The window once: the lowest ask and trade, the highest bid and trade, after t0.
            double low = kInf;
            double high = -kInf;
            for (std::ptrdiff_t i = i0 + 1; i <= i1; ++i) {
                low = std::min(low, quotes.ask[static_cast<std::size_t>(i)]);
                high = std::max(high, quotes.bid[static_cast<std::size_t>(i)]);
            }
            for (std::ptrdiff_t j = after(tms, t0), end = after(tms, t0 + w); j < end; ++j) {
                low = std::min(low, trades.price[static_cast<std::size_t>(j)]);
                high = std::max(high, trades.price[static_cast<std::size_t>(j)]);
            }
            for (const int side : {1, -1}) {
                const double market = side > 0 ? (a0 - mid1) / half0 : (mid1 - b0) / half0;
                for (const double e : orders.fill_positions) {
                    const double raw = mid0 + side * e * half0;
                    const double tick = orders.tick_size;
                    LimitFill f{.t0_ms = t0, .window_ms = w, .side = side, .fill_position = e,
                                .mid0 = mid0, .spread0 = a0 - b0, .market_cost = market};
                    double limit = 0.0;
                    double fallback = 0.0;
                    if (side > 0) {
                        limit = std::floor(raw / tick + kEps) * tick;
                        f.filled_touch = low <= limit + kEps;
                        f.filled_through = low < limit - kEps;
                        fallback = a1;
                    } else {
                        limit = std::ceil(raw / tick - kEps) * tick;
                        f.filled_touch = high >= limit - kEps;
                        f.filled_through = high > limit + kEps;
                        fallback = b1;
                    }
                    f.cost_touch = side * ((f.filled_touch ? limit : fallback) - mid1) / half0;
                    f.cost_through = side * ((f.filled_through ? limit : fallback) - mid1) / half0;
                    out.push_back(f);
                }
            }
        }
    }
    return out;
}

void FillTable::add(const Contract& contract, int day, int dte,
                    const std::vector<LimitFill>& fills) {
    for (const LimitFill& f : fills) {
        day_.push_back(day);
        dte_.push_back(dte);
        strike_.push_back(contract.strike);
        call_.push_back(contract.call ? 1 : 0);
        t0_ms_.push_back(f.t0_ms);
        window_ms_.push_back(f.window_ms);
        side_.push_back(static_cast<std::int8_t>(f.side));
        e_.push_back(f.fill_position);
        mid0_.push_back(f.mid0);
        spread0_.push_back(f.spread0);
        market_cost_.push_back(f.market_cost);
        filled_touch_.push_back(f.filled_touch);
        filled_through_.push_back(f.filled_through);
        cost_touch_.push_back(f.cost_touch);
        cost_through_.push_back(f.cost_through);
    }
}

auto FillTable::write(const std::string& path) const -> std::expected<void, std::string> {
    using arrow::field;
    const auto schema = arrow::schema(
        {field("day", arrow::int32()), field("dte", arrow::int32()),
         field("strike", arrow::float64()), field("call", arrow::int8()),
         field("t0_ms", arrow::int64()), field("w_ms", arrow::int64()),
         field("side", arrow::int8()), field("e", arrow::float64()),
         field("mid0", arrow::float64()), field("spread0", arrow::float64()),
         field("market_cost", arrow::float64()), field("filled_touch", arrow::boolean()),
         field("filled_through", arrow::boolean()), field("cost_touch", arrow::float64()),
         field("cost_through", arrow::float64())});
    const auto table = arrow::Table::Make(
        schema, {column(day_), column(dte_),
                 column(strike_), column(call_),
                 column(t0_ms_), column(window_ms_),
                 column(side_), column(e_),
                 column(mid0_), column(spread0_),
                 column(market_cost_),
                 column(filled_touch_),
                 column(filled_through_),
                 column(cost_touch_),
                 column(cost_through_)});
    return write_table(path, table);
}

// ---- Trades in cells

TradeCells::TradeCells(std::vector<CellKey> keys, std::vector<Measure> measures,
                       std::vector<std::string_view> sums)
    : sums_(std::move(sums)) {
    if (keys.empty() || keys.size() > 8) {
        throw std::invalid_argument("cells need 1 to 8 keys");
    }
    for (CellKey& k : keys) {
        if (k.measure.empty()) {
            k.measure = k.name;
        }
        const auto m = std::ranges::find(measures, k.measure, &Measure::name);
        if (m == measures.end()) {
            throw std::invalid_argument("key " + k.name + ": no measure " + k.measure);
        }
        Key key{.measure = static_cast<std::size_t>(m - measures.begin()),
                .edges = std::move(k.edges)};
        if (!key.edges.empty()) {
            if (k.labels.size() != key.edges.size() + 1 || !std::ranges::is_sorted(key.edges)) {
                throw std::invalid_argument("key " + k.name +
                                            ": sorted edges and a label more than edges");
            }
            key.labels = std::move(k.labels);
        } else if (!m->labels.empty()) {
            key.labels = m->labels;
        } else {
            key.values = true;
        }
        if (key.labels.size() >= kNull) {
            throw std::invalid_argument("key " + k.name + ": too many bins");
        }
        names_.push_back(std::move(k.name));
        keys_.push_back(std::move(key));
    }
}

auto TradeCells::bin(Key& key, double v) -> std::uint8_t {
    if (std::isnan(v)) {
        return kNull;
    }
    if (!key.edges.empty()) {
        return static_cast<std::uint8_t>(std::ranges::upper_bound(key.edges, v) -
                                         key.edges.begin());
    }
    if (!key.values) {
        return v >= 0.0 && v < static_cast<double>(key.labels.size()) ? static_cast<std::uint8_t>(v)
                                                                      : kNull;
    }
    const auto value = static_cast<std::int64_t>(std::llround(v));
    const auto [at, added] =
        key.bin_of_value.try_emplace(value, static_cast<std::uint8_t>(key.labels.size()));
    if (added) {
        if (key.labels.size() + 1 >= kNull) {
            throw std::runtime_error("a key has too many values");
        }
        key.labels.push_back(std::to_string(value));
    }
    return at->second;
}

void TradeCells::add(std::span<const double> measures, std::span<const double> values) {
    std::uint64_t code = 0;
    for (std::size_t k = 0; k < keys_.size(); ++k) {
        code |= std::uint64_t{bin(keys_[k], measures[keys_[k].measure])} << (8 * k);
    }
    std::vector<double>& sums = sums_of_[code];
    sums.resize(sums_.size(), 0.0);
    for (std::size_t i = 0; i < sums.size(); ++i) {
        if (!std::isnan(values[i])) {
            sums[i] += values[i];
        }
    }
}

void TradeCells::merge(const TradeCells& other) {
    for (const auto& [theirs, sums] : other.sums_of_) {
        std::uint64_t code = 0;
        for (std::size_t k = 0; k < keys_.size(); ++k) {
            auto b = static_cast<std::uint8_t>(theirs >> (8 * k));
            if (keys_[k].values && b != kNull) {
                b = bin(keys_[k], std::stod(other.keys_[k].labels[b]));
            }
            code |= std::uint64_t{b} << (8 * k);
        }
        std::vector<double>& mine = sums_of_[code];
        mine.resize(sums_.size(), 0.0);
        for (std::size_t i = 0; i < mine.size(); ++i) {
            mine[i] += sums[i];
        }
    }
}

auto TradeCells::write(const std::string& path) const -> std::expected<void, std::string> {
    std::vector<std::shared_ptr<arrow::Field>> fields;
    std::vector<std::shared_ptr<arrow::Array>> columns;
    for (std::size_t k = 0; k < keys_.size(); ++k) {
        arrow::StringBuilder b;
        for (const auto& [code, sums] : sums_of_) {
            const auto bin = static_cast<std::uint8_t>(code >> (8 * k));
            (void)(bin == kNull ? b.AppendNull() : b.Append(keys_[k].labels[bin]));
        }
        std::shared_ptr<arrow::Array> a;
        (void)b.Finish(&a);
        fields.push_back(arrow::field(names_[k], arrow::utf8()));
        columns.push_back(std::move(a));
    }
    for (std::size_t i = 0; i < sums_.size(); ++i) {
        std::vector<double> values;
        values.reserve(sums_of_.size());
        for (const auto& [code, sums] : sums_of_) {
            values.push_back(sums[i]);
        }
        fields.push_back(arrow::field(std::string(sums_[i]), arrow::float64()));
        columns.push_back(column(values));
    }
    return write_table(path, arrow::Table::Make(arrow::schema(fields), columns));
}

// ---- Single-leg trades

namespace {

constexpr std::array<std::string_view, 5> kKinds{"single-leg electronic", "single-leg auction",
                                                 "multi-leg", "floor", "no two-sided quote"};

auto kind_of(std::int16_t condition) -> double {
    if (condition >= 125 && condition <= 128) {
        return 1; // auction
    }
    switch (condition) {
    case 129: case 133: case 137: case 140: case 143: case 144:
        return 3; // floor
    case 130: case 131: case 132: case 134: case 135: case 136: case 138: case 139: case 141:
    case 142:
        return 2; // multi-leg
    default:
        return 0;
    }
}

auto days_between(std::chrono::year_month_day from, int yyyymmdd) -> double {
    const std::chrono::year_month_day to{
        std::chrono::year{yyyymmdd / 10000},
        std::chrono::month{static_cast<unsigned>(yyyymmdd / 100 % 100)},
        std::chrono::day{static_cast<unsigned>(yyyymmdd % 100)}};
    return static_cast<double>((std::chrono::sys_days{to} - std::chrono::sys_days{from}).count());
}

template <std::size_t N>
auto labelled(std::array<std::string_view, N> names) -> std::vector<std::string> {
    return {names.begin(), names.end()};
}

} // namespace

auto trade_cells(std::vector<CellKey> keys) -> TradeCells {
    return TradeCells(std::move(keys),
                      {{"year", {}}, {"kind", labelled(kKinds)}, {"dte", {}}, {"otm", {}},
                       {"size", {}}, {"half", {}}, {"mid", {}}, {"ms", {}}},
                      {"trades", "contracts", "e_sum", "e_size_sum", "at_mid", "inside",
                       "at_quote", "outside", "half_sum", "saved_usd_per_share_sum"});
}

void place_trades(const TradeRows& t, std::chrono::year_month_day day,
                  std::span<const int> minutes, std::span<const double> underlying,
                  TradeCells& cells) {
    const double year = static_cast<int>(day.year());
    for (std::size_t i = 0; i < t.rows(); ++i) {
        const double bid = t.bid[i];
        const double ask = t.ask[i];
        const double half = (ask - bid) / 2.0;
        const double mid = (ask + bid) / 2.0;
        const bool quoted = !(bid <= 0.0 || half <= 0.0); // a null quote is not "none": as polars
        const int before = (t.ms[i] / 60'000) - 1;
        const auto u = std::ranges::upper_bound(minutes, before) - minutes.begin() - 1;
        const double s = u < 0 ? kNaN : underlying[static_cast<std::size_t>(u)];
        const double k = t.strike[i];
        const double otm = t.call[i] != 0 ? (k / s - 1.0) * 100.0 : (1.0 - k / s) * 100.0;
        const double size = t.size[i];
        const double e = std::abs(t.price[i] - mid) / half;
        const std::array<double, 8> measures{
            year, quoted ? kind_of(t.condition[i]) : 4.0, days_between(day, t.expiry[i]), otm,
            size, half, mid, static_cast<double>(t.ms[i])};
        const auto when = [quoted](double v) { return quoted ? v : kNaN; };
        const auto count = [&](bool yes) { return std::isnan(e) ? kNaN : when(yes ? 1.0 : 0.0); };
        const std::array<double, 10> values{
            1.0, size, when(e), when(e * size), count(e < 0.001), count(e >= 0.001 && e < 0.999),
            count(e >= 0.999 && e <= 1.001), count(e > 1.001), when(half),
            when((half - std::abs(t.price[i] - mid)) * size)};
        cells.add(measures, values);
    }
}

// ---- Complex trades

namespace {

constexpr std::array<std::string_view, 6> kStructures{
    "vertical", "iron condor or fly", "butterfly", "calendar", "straddle", "strangle"};

} // namespace

auto combo_cells(std::vector<CellKey> keys) -> TradeCells {
    return TradeCells(std::move(keys),
                      {{"year", {}}, {"structure", labelled(kStructures)}, {"dte", {}},
                       {"units", {}}, {"legged", {}}, {"ms", {}}, {"condition", {}}},
                      {"trades", "units", "e_sum", "e_units_sum", "at_mid", "within_half",
                       "legged_or_worse", "legged_usd_sum", "saved_usd_sum"});
}

void place_combos(const TradeRows& t, std::chrono::year_month_day day, TradeCells& cells) {
    // The two-sided rows of complex trades, by trade (ms, exchange, condition), then contract.
    std::vector<std::size_t> rows;
    for (std::size_t i = 0; i < t.rows(); ++i) {
        if (std::ranges::contains(kComboConditions, t.condition[i]) && t.bid[i] > 0.0 &&
            t.ask[i] > t.bid[i]) {
            rows.push_back(i);
        }
    }
    const auto trade_of = [&](std::size_t i) {
        return std::tuple{t.ms[i], t.exchange[i], t.condition[i]};
    };
    const auto contract_of = [&](std::size_t i) {
        return std::tuple{t.expiry[i], t.call[i], t.strike[i]};
    };
    std::ranges::sort(rows, [&](std::size_t a, std::size_t b) {
        return std::tuple{trade_of(a), contract_of(a)} < std::tuple{trade_of(b), contract_of(b)};
    });
    struct Leg {
        int expiry;
        bool call;
        double strike, q, price, mid, half;
    };
    std::vector<Leg> legs;
    const double year = static_cast<int>(day.year());
    for (std::size_t a = 0; a < rows.size();) {
        std::size_t b = a;
        while (b < rows.size() && trade_of(rows[b]) == trade_of(rows[a])) {
            ++b;
        }
        // Its contracts: contracts traded, the size-weighted price, the NBBO's mean mid and half.
        legs.clear();
        for (std::size_t c = a; c < b;) {
            std::size_t d = c;
            double q = 0.0, paid = 0.0, mid = 0.0, half = 0.0;
            for (; d < b && contract_of(rows[d]) == contract_of(rows[c]); ++d) {
                const std::size_t i = rows[d];
                q += t.size[i];
                paid += t.price[i] * t.size[i];
                mid += (t.bid[i] + t.ask[i]) / 2.0;
                half += (t.ask[i] - t.bid[i]) / 2.0;
            }
            const double n = static_cast<double>(d - c);
            const std::size_t i = rows[c];
            legs.push_back({t.expiry[i], t.call[i] != 0, t.strike[i], q, paid / q, mid / n,
                            half / n});
            c = d;
        }
        const std::size_t i0 = rows[a];
        a = b;
        const bool one_expiry =
            std::ranges::all_of(legs, [&](const Leg& l) { return l.expiry == legs[0].expiry; });
        const auto calls = std::ranges::count(legs, true, &Leg::call);
        const std::size_t n = legs.size();
        const bool one_right = calls == 0 || calls == static_cast<long>(n);
        double unit = legs[0].q;
        for (const Leg& l : legs) {
            unit = std::min(unit, l.q);
        }
        const auto ratio = [&](std::size_t k) { return legs[k].q / unit; };
        std::array<double, 4> sign{};
        int structure = -1;
        const bool pair = n == 2 && ratio(0) == 1 && ratio(1) == 1;
        if (!one_expiry) { // legs by expiry: the near one first
            if (pair && one_right && legs[0].strike == legs[1].strike) {
                structure = 3;
                sign = {-1, 1};
            }
        } else if (pair && one_right) {
            structure = 0;
            sign = {1, -1};
        } else if (pair && legs[0].strike == legs[1].strike) { // the put first
            structure = 4;
            sign = {1, 1};
        } else if (pair && legs[0].strike < legs[1].strike) {
            structure = 5;
            sign = {1, 1};
        } else if (n == 4 && calls == 2 && ratio(0) == 1 && ratio(1) == 1 && ratio(2) == 1 &&
                   ratio(3) == 1 && legs[1].strike <= legs[2].strike) { // puts first, by strike
            structure = 1;
            sign = {-1, 1, 1, -1};
        } else if (n == 3 && one_right && ratio(0) == 1 && ratio(1) == 2 && ratio(2) == 1) {
            structure = 2;
            sign = {1, -2, 1};
        }
        if (structure < 0) {
            continue;
        }
        double net = 0.0, net_mid = 0.0, legged = 0.0;
        for (std::size_t k = 0; k < n; ++k) {
            net += sign[k] * legs[k].price;
            net_mid += sign[k] * legs[k].mid;
            legged += std::abs(sign[k]) * legs[k].half;
        }
        const double e = std::abs(net - net_mid) / legged;
        const std::array<double, 7> measures{year, static_cast<double>(structure),
                                             days_between(day, legs[0].expiry), unit, legged,
                                             static_cast<double>(t.ms[i0]),
                                             static_cast<double>(t.condition[i0])};
        const std::array<double, 9> values{1.0, unit, e, e * unit, e < 0.001 ? 1.0 : 0.0,
                                           e <= 0.5 + kEps ? 1.0 : 0.0, // halfway, at any rounding
                                           e >= 0.999 ? 1.0 : 0.0,
                                           legged * unit,
                                           (legged - std::abs(net - net_mid)) * unit};
        cells.add(measures, values);
    }
}

// ---- Order flow

namespace {

constexpr std::int32_t kOpenMs = ((9 * 60) + 30) * 60'000;
constexpr double kMultiplier = 100.0;
constexpr double kMsPerYear = 365.0 * 86'400'000.0;

struct Greeks {
    double iv = kNaN;
    double delta = 0.0;
    double gamma = 0.0;
    double vega = 0.0;
};

auto greeks_of(bool call, double spot, double strike, double years, double mid) -> Greeks {
    if (!(spot > 0.0) || !(mid > 0.0) || !(years > 0.0)) {
        return {};
    }
    const double iv = quant::black_implied_vol(mid, spot, strike, years, 0.0, call);
    if (!(iv > 0.0)) {
        return {};
    }
    const quant::BlackGreeks g = quant::black_greeks(call, spot, spot, strike, years, 0.0, iv);
    return {.iv = iv, .delta = g.delta, .gamma = g.gamma, .vega = g.vega};
}

template <typename Builder, typename Row, typename Get>
auto column(const std::vector<Row>& rows, Get get) -> std::shared_ptr<arrow::Array> {
    Builder builder;
    (void)builder.Reserve(static_cast<std::int64_t>(rows.size()));
    for (const Row& r : rows) {
        builder.UnsafeAppend(get(r));
    }
    std::shared_ptr<arrow::Array> out;
    (void)builder.Finish(&out);
    return out;
}

/** A table of named columns, each an arrow type and its array. */
struct Columns {
    std::vector<std::shared_ptr<arrow::Field>> fields;
    std::vector<std::shared_ptr<arrow::Array>> arrays;
    void add(const std::string& name, std::shared_ptr<arrow::DataType> type,
             std::shared_ptr<arrow::Array> array) {
        fields.push_back(arrow::field(name, std::move(type)));
        arrays.push_back(std::move(array));
    }
    [[nodiscard]] auto table() const { return arrow::Table::Make(arrow::schema(fields), arrays); }
};

} // namespace

void FlowTable::add(const TradeRows& t, std::chrono::year_month_day day,
                    std::span<const int> minutes, std::span<const double> underlying) {
    const auto date =
        static_cast<std::int32_t>(std::chrono::sys_days{day}.time_since_epoch().count());
    const int day_number = (static_cast<int>(day.year()) * 10000) +
                           (static_cast<int>(static_cast<unsigned>(day.month())) * 100) +
                           static_cast<int>(static_cast<unsigned>(day.day()));
    // Greeks a contract a minute: at the first trade's mid of that minute.
    std::unordered_map<std::uint64_t, Greeks> greeks;
    greeks.reserve(t.rows() / 4);
    // A cell's index: ((bin + 1) x 5 + dte) x 12 + call x 6 + multi x 3 + size; few, so dense.
    constexpr std::size_t kCells = 28 * 5 * 12;
    std::vector<std::array<double, kSums>> sums(kCells);
    std::vector<bool> used(kCells, false);
    std::unordered_map<std::uint64_t, ContractNet> by_contract; // single-leg trades
    by_contract.reserve(8192);
    int last_expiry = 0;
    int dte = 0;
    for (std::size_t i = 0; i < t.rows(); ++i) {
        const double bid = t.bid[i];
        const double ask = t.ask[i];
        const double mid = (bid + ask) / 2.0;
        const bool quoted = two_sided(bid, ask);
        const double price = t.price[i];
        const int sign = !quoted ? 0 : price > mid + kEps ? 1 : price < mid - kEps ? -1 : 0;
        if (t.expiry[i] != last_expiry) { // a day's rows come an expiry at a time
            last_expiry = t.expiry[i];
            dte = static_cast<int>(days_between(day, last_expiry));
        }
        const bool call = t.call[i] != 0;
        const auto strike_milli = static_cast<std::int32_t>(std::lround(t.strike[i] * 1000.0));
        const int minute = t.ms[i] / 60'000;
        const auto u = std::ranges::upper_bound(minutes, minute - 1) - minutes.begin() - 1;
        const double spot = u < 0 ? kNaN : underlying[static_cast<std::size_t>(u)];
        Greeks g;
        const double to_close = (dte * 86'400'000.0) + (16.0 * 3'600'000.0) - t.ms[i];
        if (quoted && dte <= 45 && to_close >= 300'000.0) {
            const std::uint64_t key = (contract_key(t.expiry[i], strike_milli, call) << 11) |
                                      static_cast<std::uint64_t>(minute & 0x7FF);
            auto [it, fresh] = greeks.try_emplace(key);
            if (fresh) {
                it->second = greeks_of(call, spot, t.strike[i], to_close / kMsPerYear, mid);
            }
            g = it->second;
        }
        const double size = t.size[i];
        const bool multi = kind_of(t.condition[i]) >= 2.0;
        const int bin =
            t.ms[i] < kOpenMs ? -1 : std::min(26, static_cast<int>((t.ms[i] - kOpenMs) / 900'000));
        const int dte_class = dte <= 1 ? dte : dte <= 7 ? 2 : dte <= 45 ? 3 : 4;
        const int size_class = size < 10 ? 0 : size < 100 ? 1 : 2;
        const std::size_t cell =
            (((static_cast<std::size_t>(bin + 1) * 5) + dte_class) * 12) + (call ? 6 : 0) +
            (multi ? 3 : 0) + size_class;
        used[cell] = true;
        std::array<double, kSums>& s = sums[cell];
        const double contracts = size * kMultiplier; // the shares a contract delivers
        s[0] += 1.0;
        s[1] += size;
        s[2] += sign > 0 ? size : 0.0;
        s[3] += sign < 0 ? size : 0.0;
        s[4] += sign * contracts * g.delta;
        s[5] += contracts * std::abs(g.delta);
        s[6] += std::isnan(g.iv) ? 0.0 : sign * contracts * g.gamma * spot * spot / 100.0;
        s[7] += sign * contracts * g.vega;
        s[8] += sign * contracts * price;
        if (multi) {
            continue;
        }
        ContractNet& c = by_contract[contract_key(t.expiry[i], strike_milli, call)];
        c = {.day = day_number, .expiry = t.expiry[i], .strike_milli = strike_milli,
             .call = call, .net = c.net + (sign * t.size[i]), .volume = c.volume + t.size[i]};
        if (t.size[i] >= big_size_) {
            big_.push_back({.day = date, .ms = t.ms[i], .size = t.size[i],
                            .dte = static_cast<std::int16_t>(dte), .condition = t.condition[i],
                            .exchange = t.exchange[i], .call = call,
                            .sign = static_cast<std::int8_t>(sign), .strike = t.strike[i],
                            .price = price, .bid = bid, .ask = ask, .spot = spot, .iv = g.iv,
                            .delta = g.delta, .gamma = g.gamma});
        }
    }
    for (const auto& [key, c] : by_contract) {
        contracts_.push_back(c);
    }
    for (std::size_t cell = 0; cell < kCells; ++cell) {
        if (used[cell]) {
            cells_.push_back({.day = date,
                              .bin = static_cast<std::int8_t>(static_cast<int>(cell / 60) - 1),
                              .dte = static_cast<std::int8_t>(cell / 12 % 5),
                              .call = cell % 12 / 6 != 0,
                              .multi = cell % 6 / 3 != 0,
                              .size = static_cast<std::int8_t>(cell % 3),
                              .sums = sums[cell]});
        }
    }
}

void FlowTable::merge(FlowTable&& o) {
    cells_.insert(cells_.end(), o.cells_.begin(), o.cells_.end());
    big_.insert(big_.end(), o.big_.begin(), o.big_.end());
    contracts_.insert(contracts_.end(), o.contracts_.begin(), o.contracts_.end());
}

auto FlowTable::write(const std::string& path) const -> std::expected<void, std::string> {
    Columns c;
    c.add("day", arrow::date32(), column<arrow::Date32Builder>(cells_, [](const Cell& r) {
              return r.day;
          }));
    c.add("bin", arrow::int8(), column<arrow::Int8Builder>(cells_, [](const Cell& r) {
              return r.bin;
          }));
    c.add("dte", arrow::int8(), column<arrow::Int8Builder>(cells_, [](const Cell& r) {
              return r.dte;
          }));
    c.add("call", arrow::int8(), column<arrow::Int8Builder>(cells_, [](const Cell& r) {
              return static_cast<std::int8_t>(r.call);
          }));
    c.add("multi", arrow::int8(), column<arrow::Int8Builder>(cells_, [](const Cell& r) {
              return static_cast<std::int8_t>(r.multi);
          }));
    c.add("size", arrow::int8(), column<arrow::Int8Builder>(cells_, [](const Cell& r) {
              return r.size;
          }));
    static constexpr std::array<const char*, kSums> kNames{
        "trades",           "contracts", "buy",      "sell",       "delta_shares",
        "abs_delta_shares", "gamma_usd", "vega_usd", "premium_usd"};
    for (std::size_t k = 0; k < kSums; ++k) {
        c.add(kNames[k], arrow::float64(),
              column<arrow::DoubleBuilder>(cells_, [k](const Cell& r) { return r.sums[k]; }));
    }
    return write_table(path, c.table());
}

auto FlowTable::write_big(const std::string& path) const -> std::expected<void, std::string> {
    Columns c;
    c.add("day", arrow::date32(),
          column<arrow::Date32Builder>(big_, [](const BigTrade& r) { return r.day; }));
    constexpr std::array<std::pair<const char*, std::int32_t BigTrade::*>, 2> kInt32s{
        {{"ms", &BigTrade::ms}, {"size", &BigTrade::size}}};
    for (const auto& [name, member] : kInt32s) {
        c.add(name, arrow::int32(), column<arrow::Int32Builder>(big_, [member](const BigTrade& r) {
                  return r.*member;
              }));
    }
    constexpr std::array<std::pair<const char*, std::int16_t BigTrade::*>, 3> kInt16s{
        {{"dte", &BigTrade::dte}, {"condition", &BigTrade::condition},
         {"exchange", &BigTrade::exchange}}};
    for (const auto& [name, member] : kInt16s) {
        c.add(name, arrow::int16(), column<arrow::Int16Builder>(big_, [member](const BigTrade& r) {
                  return r.*member;
              }));
    }
    c.add("call", arrow::int8(), column<arrow::Int8Builder>(big_, [](const BigTrade& r) {
              return static_cast<std::int8_t>(r.call);
          }));
    c.add("sign", arrow::int8(),
          column<arrow::Int8Builder>(big_, [](const BigTrade& r) { return r.sign; }));
    constexpr std::array<std::pair<const char*, double BigTrade::*>, 8> kDoubles{
        {{"strike", &BigTrade::strike},
         {"price", &BigTrade::price},
         {"bid", &BigTrade::bid},
         {"ask", &BigTrade::ask},
         {"spot", &BigTrade::spot},
         {"iv", &BigTrade::iv},
         {"delta", &BigTrade::delta},
         {"gamma", &BigTrade::gamma}}};
    for (const auto& [name, member] : kDoubles) {
        c.add(name, arrow::float64(), column<arrow::DoubleBuilder>(big_, [member](const BigTrade& r) {
                  return r.*member;
              }));
    }
    return write_table(path, c.table());
}

auto FlowTable::write_contracts(const std::string& path) const
    -> std::expected<void, std::string> {
    Columns c;
    constexpr std::array<std::pair<const char*, std::int32_t ContractNet::*>, 5> kInts{
        {{"day", &ContractNet::day},
         {"expiry", &ContractNet::expiry},
         {"strike_milli", &ContractNet::strike_milli},
         {"net", &ContractNet::net},
         {"volume", &ContractNet::volume}}};
    for (const auto& [name, member] : kInts) {
        c.add(name, arrow::int32(),
              column<arrow::Int32Builder>(contracts_, [member](const ContractNet& r) {
                  return r.*member;
              }));
    }
    c.add("call", arrow::int8(), column<arrow::Int8Builder>(contracts_, [](const ContractNet& r) {
              return static_cast<std::int8_t>(r.call);
          }));
    return write_table(path, c.table());
}

// ---- Within a minute

void sample_intraminute(const QuoteSeries& q, std::span<const std::int64_t> minutes,
                        std::uint64_t seed, IntraminuteRows& out) {
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<std::int64_t> within(0, 59'999);
    const std::span<const std::int64_t> ms(q.ms);
    const auto quote = [&](std::int64_t t) -> std::pair<double, double> {
        const std::ptrdiff_t i = at_or_before(ms, t);
        return i < 0 ? std::pair{0.0, 0.0}
                     : std::pair{q.bid[static_cast<std::size_t>(i)],
                                 q.ask[static_cast<std::size_t>(i)]};
    };
    for (std::size_t m = 0; m < minutes.size(); ++m) {
        const std::int64_t t = minutes[m];
        const std::int64_t r = t + within(rng);
        const auto [bm, am] = quote(t);
        const auto [bd, ad] = quote(t + 1'000);
        const auto [br, ar] = quote(r);
        const auto [bn, an] = quote(t + 60'000);
        if (!two_sided(bm, am) || !two_sided(bd, ad) || !two_sided(br, ar) || !two_sided(bn, an)) {
            continue;
        }
        const auto f = [](double v) { return static_cast<float>(v); };
        out.minute.push_back(static_cast<std::int16_t>(m));
        out.mid_m.push_back(f((bm + am) / 2.0));
        out.spread_m.push_back(f(am - bm));
        out.spread_r.push_back(f(ar - br));
        out.ask_d.push_back(f(ad));
        out.bid_d.push_back(f(bd));
        out.ask_n.push_back(f(an));
        out.bid_n.push_back(f(bn));
        out.mid_d.push_back(f((bd + ad) / 2.0));
        out.mid_r.push_back(f((br + ar) / 2.0));
        out.mid_n.push_back(f((bn + an) / 2.0));
    }
}

auto write_intraminute(const std::string& path,
                       const std::vector<std::pair<std::chrono::year_month_day, int>>& days,
                       const std::vector<IntraminuteRows>& rows)
    -> std::expected<void, std::string> {
    arrow::Date32Builder day;
    arrow::Int8Builder dte;
    arrow::Int16Builder minute;
    std::array<arrow::FloatBuilder, 10> floats;
    for (std::size_t d = 0; d < rows.size(); ++d) {
        const IntraminuteRows& r = rows[d];
        const auto date = static_cast<std::int32_t>(
            std::chrono::sys_days{days[d].first}.time_since_epoch().count());
        for (std::size_t i = 0; i < r.minute.size(); ++i) {
            (void)day.Append(date);
            (void)dte.Append(static_cast<std::int8_t>(days[d].second));
        }
        (void)minute.AppendValues(r.minute);
        const std::array<const std::vector<float>*, 10> columns{
            &r.mid_m, &r.spread_m, &r.spread_r, &r.ask_d, &r.bid_d,
            &r.ask_n, &r.bid_n,    &r.mid_d,    &r.mid_r, &r.mid_n};
        for (std::size_t c = 0; c < columns.size(); ++c) {
            (void)floats[c].AppendValues(*columns[c]);
        }
    }
    static constexpr std::array<const char*, 10> kNames{"mid_m", "spread_m", "spread_r", "ask_d",
                                                        "bid_d", "ask_n",    "bid_n",    "mid_d",
                                                        "mid_r", "mid_n"};
    std::vector<std::shared_ptr<arrow::Field>> fields{arrow::field("day", arrow::date32()),
                                                      arrow::field("dte", arrow::int8()),
                                                      arrow::field("minute", arrow::int16())};
    std::vector<std::shared_ptr<arrow::Array>> columns(3);
    (void)day.Finish(&columns[0]);
    (void)dte.Finish(&columns[1]);
    (void)minute.Finish(&columns[2]);
    for (std::size_t c = 0; c < floats.size(); ++c) {
        fields.push_back(arrow::field(kNames[c], arrow::float32()));
        columns.emplace_back();
        (void)floats[c].Finish(&columns.back());
    }
    return write_table(path, arrow::Table::Make(arrow::schema(fields), columns));
}

} // namespace research
