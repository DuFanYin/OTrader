#include "research/structures.hpp"

#include "model/expiry.hpp"
#include "model/symbol.hpp"
#include "research/offline_data.hpp"

#include <algorithm>
#include <arrow/api.h>
#include <chrono>
#include <cmath>
#include <deque>
#include <filesystem>
#include <functional>
#include <future>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <type_traits>
#include <unordered_map>

namespace research {

namespace {

using std::chrono::year_month_day;
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

auto trading_days_after(const model::SessionCalendar& calendar, year_month_day d, int n)
    -> year_month_day {
    for (; n > 0; --n) {
        d = calendar.next_trading_day(d);
    }
    return d;
}

auto trading_days_before(const model::SessionCalendar& calendar, year_month_day d, int n)
    -> year_month_day {
    for (; n > 0; --n) {
        do {
            d = year_month_day{std::chrono::sys_days{d} - std::chrono::days{1}};
        } while (!calendar.is_trading_day(d));
    }
    return d;
}

struct Leg {
    std::string symbol;
    std::string chain;
    model::OptionId id;
    int ratio = 0;
    double multiplier = 0.0;
};

struct PendingExit {
    year_month_day date{};
    int minute = 0; // since the open; at an expiry, its settlement's
    bool at_expiry = false;
    bool done = false;
    StructureTable::Row values; // its exit's fields
};

struct Entry {
    std::size_t structure = 0;
    StructureTable::Row row; // its entry's fields
    std::vector<Leg> legs;
    std::vector<PendingExit> exits;
};

/** The legs now: their mid, what opening them at the quotes costs (buying at the ask, selling at
 * the bid) and what closing takes in, each NaN when a leg is not listed or not quoted on that side;
 * a leg expiring on `settling` at its intrinsic value at `settle_price`. */
struct Value {
    double mid = 0.0;
    double open = 0.0;
    double close = 0.0;
};

auto value_of(const model::Portfolio& portfolio, const std::vector<Leg>& legs,
              std::optional<year_month_day> settling, double settle_price) -> Value {
    Value v;
    for (const Leg& leg : legs) {
        const double units = leg.ratio * leg.multiplier;
        if (settling && leg.id.expiry == *settling) {
            const double intrinsic = model::intrinsic(leg.id, settle_price) * units;
            v.mid += intrinsic;
            v.open += intrinsic;
            v.close += intrinsic;
            continue;
        }
        const std::optional<model::Option> option = portfolio.option(leg.symbol);
        if (!option || (option->bid() <= 0.0 && option->ask() <= 0.0)) {
            return {.mid = kNaN, .open = kNaN, .close = kNaN};
        }
        const double opening = leg.ratio > 0 ? option->ask() : option->bid();
        const double closing = leg.ratio > 0 ? option->bid() : option->ask();
        v.mid += option->mid() * units;
        v.open += opening > 0.0 ? opening * units : kNaN;
        v.close += (leg.ratio < 0 && closing <= 0.0) ? kNaN : closing * units;
    }
    return v;
}

auto underlying_mid(const model::Portfolio& portfolio) -> double {
    const std::optional<model::Underlying> u = portfolio.underlying();
    return u && u->mid() > 0.0 ? u->mid() : kNaN;
}

/** Each leg's option now by its rule on its chain, quoted on the side it opens; none when a leg
 * cannot be (or two legs are one contract, which no venue takes as a combo). */
auto pick_legs(const model::Portfolio& portfolio, const Structure& s,
               const std::vector<std::string>& chains) -> std::optional<std::vector<Leg>> {
    std::vector<Leg> legs;
    for (std::size_t k = 0; k < s.legs.size(); ++k) {
        const StructureLeg& leg = s.legs[k];
        const model::LegRule& rule = leg.rule;
        const std::optional<model::Chain> chain = portfolio.chain(chains[k]);
        std::optional<model::Option> option;
        if (chain && leg.strike_of >= 0) {
            option =
                chain->option(rule.right, legs[static_cast<std::size_t>(leg.strike_of)].id.strike);
        } else if (chain) {
            option = model::leg_option(*chain, rule);
        }
        if (!option || (rule.ratio > 0 ? option->ask() : option->bid()) <= 0.0) {
            return std::nullopt;
        }
        const std::optional<model::OptionId> id = model::parse_option_symbol(option->symbol());
        if (!id ||
            std::ranges::any_of(legs, [&](const Leg& l) { return l.symbol == option->symbol(); })) {
            return std::nullopt;
        }
        legs.push_back({.symbol = option->symbol(), .chain = chains[k], .id = *id,
                        .ratio = rule.ratio, .multiplier = option->multiplier()});
    }
    return legs;
}

auto validate(const ScreenSpec& spec) -> std::string {
    if (spec.structures.empty()) {
        return "no structures";
    }
    for (const Structure& s : spec.structures) {
        const std::string what = "structure '" + s.name + "': ";
        if (s.legs.empty() || s.entry_minutes.empty() || s.exits.empty()) {
            return what + "it needs legs, entry minutes and exits";
        }
        for (std::size_t k = 0; k < s.legs.size(); ++k) {
            const int earlier = static_cast<int>(k);
            if (s.legs[k].expiry_of >= earlier || s.legs[k].strike_of >= earlier) {
                return what + "a leg takes an expiry or a strike only from a leg before it";
            }
        }
    }
    return {};
}

/** A day's bars by minute since the open, read as a backtest reads them: decided at a minute's bar,
 * done at the next bar's quotes; an expiry settled at the last bar at or before its time. */
class DayBars {
  public:
    DayBars(const adapters::HistoricalData& day, const model::SessionCalendar& calendar,
            year_month_day today)
        : minutes_(day.minutes()),
          settle_minute_(calendar.minutes_since_open(calendar.expiry_time(today))) {
        for (std::size_t i = 0; i < day.minutes(); ++i) {
            bar_of_[calendar.minutes_since_open(day.time(i))] = i;
        }
    }

    [[nodiscard]] std::optional<std::size_t> exactly(int minute) const {
        const auto it = bar_of_.find(minute);
        return it == bar_of_.end() ? std::nullopt : std::optional(it->second);
    }
    [[nodiscard]] std::optional<std::size_t> next(std::optional<std::size_t> bar) const {
        return bar && *bar + 1 < minutes_ ? std::optional(*bar + 1) : std::nullopt;
    }
    [[nodiscard]] std::optional<std::size_t> exit(bool at_expiry, int minute) const {
        if (!at_expiry) {
            return next(exactly(minute));
        }
        const auto it = bar_of_.upper_bound(settle_minute_);
        return it == bar_of_.begin() ? std::nullopt : std::optional(std::prev(it)->second);
    }
    /** The bars a day of the screen reads, whatever is open: every entry's and the next, every
     * exit's (an open entry's exits are its structure's). */
    [[nodiscard]] std::vector<std::size_t> read(const ScreenSpec& spec) const {
        std::set<std::size_t> bars;
        const auto add = [&](std::optional<std::size_t> bar) {
            if (bar) {
                bars.insert(*bar);
            }
        };
        for (const Structure& st : spec.structures) {
            for (const int minute : st.entry_minutes) {
                add(exactly(minute));
                add(next(exactly(minute)));
            }
            for (const StructureExit& x : st.exits) {
                add(exit(x.at == StructureExit::At::kExpiry, x.minute));
            }
        }
        return {bars.begin(), bars.end()};
    }

  private:
    std::size_t minutes_;
    int settle_minute_;
    std::map<int, std::size_t> bar_of_;
};

/** Each structure's chains today by its legs' rules; none for one a leg has no chain for. */
auto entry_chains(const ScreenSpec& spec, const model::Portfolio& portfolio)
    -> std::vector<std::vector<std::string>> {
    std::vector<std::vector<std::string>> out(spec.structures.size());
    for (std::size_t s = 0; s < spec.structures.size(); ++s) {
        const Structure& st = spec.structures[s];
        std::vector<std::string> chains(st.legs.size());
        bool all = true;
        for (std::size_t k = 0; k < st.legs.size() && all; ++k) {
            const int from = st.legs[k].expiry_of;
            chains[k] = from >= 0 ? chains[static_cast<std::size_t>(from)]
                                  : model::leg_chain(portfolio, st.legs[k].rule);
            all = !chains[k].empty();
        }
        if (all) {
            out[s] = std::move(chains);
        }
    }
    return out;
}

/** A day loaded for the screen, on a thread of its own: its entries' chains, priced at the bars
 * it reads only (what the entries leaving it hold is loaded when it is run, at the same bars). */
auto load_screen_day(const ScreenSpec& spec, const std::string& file) -> adapters::LoadedDay {
    adapters::LoadedDay day = adapters::load_day(file, spec.risk_free_rate, spec.iv_price_mode,
                                                 spec.cache_dir, spec.store_chains);
    const year_month_day today{std::chrono::floor<std::chrono::days>(day->day())};
    if (!day->has_data() || (spec.holdout != 0 && yyyymmdd(today) >= spec.holdout)) {
        return day;
    }
    model::Portfolios portfolios;
    day->build_portfolio(portfolios);
    const model::Portfolio* portfolio = portfolios.find_portfolio(day->portfolio_name());
    if (portfolio == nullptr) {
        return day;
    }
    day->set_priced_minutes(DayBars(*day, portfolio->calendar(), today).read(spec));
    std::set<std::string> chains;
    for (const std::vector<std::string>& c : entry_chains(spec, *portfolio)) {
        chains.insert(c.begin(), c.end());
    }
    day->load_chains(std::vector<std::string>(chains.begin(), chains.end()));
    return day;
}


} // namespace

// ---- StructureTable

StructureTable::StructureTable(const std::vector<Structure>& structures)
    : structures_(structures) {}

void StructureTable::add(const Row& row, const std::vector<double>& strikes,
                         const std::vector<int>& expiries, const std::vector<int>& ratios,
                         const std::vector<bool>& calls) {
    const Structure& s = structures_[static_cast<std::size_t>(row.structure)];
    structure_.push_back(row.structure);
    name_.push_back(s.name);
    entry_day_.push_back(row.entry_day);
    entry_minute_.push_back(row.entry_minute);
    exit_.push_back(s.exits[static_cast<std::size_t>(row.exit)].label);
    exit_day_.push_back(row.exit_day);
    exit_minute_.push_back(row.exit_minute);
    settled_.push_back(row.settled);
    underlying0_.push_back(row.underlying0);
    forward0_.push_back(row.forward0);
    move0_.push_back(row.move0);
    entry_mid_.push_back(row.entry_mid);
    entry_quotes_.push_back(row.entry_quotes);
    entry_delta_.push_back(row.entry_delta);
    entry_vega_.push_back(row.entry_vega);
    exit_mid_.push_back(row.exit_mid);
    exit_quotes_.push_back(row.exit_quotes);
    underlying1_.push_back(row.underlying1);
    strikes_.push_back(strikes);
    expiries_.push_back(expiries);
    ratios_.push_back(ratios);
    calls_.push_back(calls);
}


auto StructureTable::write(const std::string& path) const -> std::expected<void, std::string> {
    using arrow::field;
    const auto f64 = arrow::float64();
    const auto schema = arrow::schema({field("structure", arrow::int32()),
                                       field("name", arrow::utf8()),
                                       field("entry_day", arrow::int32()),
                                       field("entry_minute", arrow::int32()),
                                       field("exit", arrow::utf8()),
                                       field("exit_day", arrow::int32()),
                                       field("exit_minute", arrow::int32()),
                                       field("settled", arrow::boolean()),
                                       field("strikes", arrow::list(f64)),
                                       field("expiries", arrow::list(arrow::int32())),
                                       field("ratios", arrow::list(arrow::int32())),
                                       field("calls", arrow::list(arrow::boolean())),
                                       field("underlying0", f64),
                                       field("forward0", f64),
                                       field("move0", f64),
                                       field("entry_mid", f64),
                                       field("entry_quotes", f64),
                                       field("entry_delta", f64),
                                       field("entry_vega", f64),
                                       field("exit_mid", f64),
                                       field("exit_quotes", f64),
                                       field("underlying1", f64)});
    using arrow::DoubleBuilder;
    using arrow::Int32Builder;
    const auto table = arrow::Table::Make(
        schema,
        {column(structure_), column(name_),
         column(entry_day_), column(entry_minute_),
         column(exit_), column(exit_day_),
         column(exit_minute_), column(settled_),
         list_column(strikes_), list_column(expiries_),
         list_column(ratios_), list_column(calls_),
         column(underlying0_), column(forward0_),
         column(move0_), column(entry_mid_),
         column(entry_quotes_), column(entry_delta_),
         column(entry_vega_), column(exit_mid_),
         column(exit_quotes_), column(underlying1_)});
    return write_table(path, table);
}

// ---- The screen

auto screen_structures(const ScreenSpec& spec, StructureTable& table)
    -> std::expected<std::vector<std::string>, std::string> {
    if (std::string why = validate(spec); !why.empty()) {
        return std::unexpected(std::move(why));
    }
    adapters::Replay replay;
    std::vector<Entry> open;
    std::vector<std::string> errors;

    const auto write = [&](const Entry& e) {
        std::vector<double> strikes;
        std::vector<int> expiries, ratios;
        std::vector<bool> calls;
        for (const Leg& leg : e.legs) {
            strikes.push_back(leg.id.strike);
            expiries.push_back(yyyymmdd(leg.id.expiry));
            ratios.push_back(leg.ratio);
            calls.push_back(leg.id.type == model::OptionType::CALL);
        }
        for (std::size_t x = 0; x < e.exits.size(); ++x) {
            StructureTable::Row row = e.row;
            const StructureTable::Row& got = e.exits[x].values;
            row.exit = static_cast<int>(x);
            row.exit_day = got.exit_day;
            row.exit_minute = got.exit_minute;
            row.settled = got.settled;
            row.exit_mid = got.exit_mid;
            row.exit_quotes = got.exit_quotes;
            row.underlying1 = got.underlying1;
            table.add(row, strikes, expiries, ratios, calls);
        }
    };
    const auto flush = [&] {
        std::erase_if(open, [&](const Entry& e) {
            const bool all = std::ranges::all_of(e.exits, &PendingExit::done);
            if (all) {
                write(e);
            }
            return all;
        });
    };
    const auto not_reached = [](PendingExit& x) { // when it was due, no values
        x.done = true;
        x.values.exit_day = yyyymmdd(x.date);
        x.values.exit_minute = x.minute;
        x.values.exit_mid = x.values.exit_quotes = x.values.underlying1 = kNaN;
    };

    // The days loaded ahead of their turn, up to days_ahead at a time.
    std::deque<std::future<adapters::LoadedDay>> loading;
    std::size_t queued = 0;
    for (const std::string& file : spec.days) {
        for (; queued < spec.days.size() &&
               loading.size() < std::max<std::size_t>(spec.days_ahead, 1);
             ++queued) {
            loading.push_back(std::async(std::launch::async, load_screen_day, std::cref(spec),
                                         std::cref(spec.days[queued])));
        }
        const adapters::LoadedDay day = loading.front().get();
        loading.pop_front();
        if (!day->has_data()) {
            errors.push_back(file + ": " + day->load_error());
            continue;
        }
        const year_month_day today{std::chrono::floor<std::chrono::days>(day->day())};
        if (spec.holdout != 0 && yyyymmdd(today) >= spec.holdout) {
            errors.push_back(file + ": in the holdout, not read");
            continue;
        }
        for (Entry& e : open) { // an exit on a day there is no file of is not reached
            for (PendingExit& x : e.exits) {
                if (!x.done && x.date < today) {
                    not_reached(x);
                }
            }
        }
        flush();
        replay.roll(*day);
        replay.load(*day, day->loaded_chains()); // the entries' chains, loaded with the day
        model::Portfolio* portfolio = replay.portfolios().find_portfolio(day->portfolio_name());
        if (portfolio == nullptr) {
            errors.push_back(file + ": no portfolio " + day->portfolio_name());
            continue;
        }
        const model::SessionCalendar& calendar = portfolio->calendar();

        // The chains: today's entries' by their rules (loaded ahead), and those of entries
        // leaving today, priced at the same bars.
        const std::vector<std::vector<std::string>> entry_chain = entry_chains(spec, *portfolio);
        std::set<std::string> wanted;
        for (const std::vector<std::string>& chains : entry_chain) {
            wanted.insert(chains.begin(), chains.end());
        }
        for (const Entry& e : open) { // a leg settled at an exit needs no quote
            for (const PendingExit& x : e.exits) {
                if (!x.done && x.date == today) {
                    for (const Leg& leg : e.legs) {
                        if (!x.at_expiry || leg.id.expiry != today) {
                            wanted.insert(leg.chain);
                        }
                    }
                }
            }
        }
        const DayBars bars(*day, calendar, today);
        day->set_priced_minutes(bars.read(spec));
        replay.load(*day, std::vector<std::string>(wanted.begin(), wanted.end()));
        const auto exit_bar = [&](const PendingExit& x) {
            return bars.exit(x.at_expiry, x.minute);
        };

        struct Work {
            std::vector<std::pair<std::size_t, std::size_t>> exits; // (entry, exit) valued here
            std::vector<std::size_t> picks;   // structures whose legs are picked here
            std::vector<std::size_t> fills;   // waiting entries opened here
        };
        std::map<std::size_t, Work> work;
        for (std::size_t e = 0; e < open.size(); ++e) {
            for (std::size_t x = 0; x < open[e].exits.size(); ++x) {
                const PendingExit& pending = open[e].exits[x];
                if (!pending.done && pending.date == today) {
                    if (const std::optional<std::size_t> bar = exit_bar(pending)) {
                        work[*bar].exits.emplace_back(e, x);
                    }
                }
            }
        }
        for (std::size_t s = 0; s < spec.structures.size(); ++s) {
            if (entry_chain[s].empty()) {
                continue;
            }
            for (const int minute : spec.structures[s].entry_minutes) {
                if (const std::optional<std::size_t> bar = bars.exactly(minute); bars.next(bar)) {
                    work[*bar].picks.push_back(s);
                }
            }
        }

        std::vector<Entry> waiting; // legs picked, opened at the next bar
        for (auto& [bar, w] : work) { // a map: bars added while iterating, later, are visited
            replay.apply(day, bar);
            const int minute = calendar.minutes_since_open(day->time(bar));
            const double underlying = underlying_mid(*portfolio);
            for (const auto& [e, x] : w.exits) {
                PendingExit& pending = open[e].exits[x];
                const std::optional<year_month_day> settling =
                    pending.at_expiry ? std::optional(today) : std::nullopt;
                const Value v = value_of(*portfolio, open[e].legs, settling, underlying);
                pending.done = true;
                pending.values.exit_day = yyyymmdd(today);
                pending.values.exit_minute = pending.at_expiry ? minute : pending.minute;
                pending.values.settled = pending.at_expiry;
                pending.values.exit_mid = v.mid;
                pending.values.exit_quotes = v.close;
                pending.values.underlying1 = underlying;
            }
            for (const std::size_t i : w.fills) {
                Entry& entry = waiting[i];
                const Structure& st = spec.structures[entry.structure];
                const Value v = value_of(*portfolio, entry.legs, std::nullopt, 0.0);
                double delta = 0.0;
                double vega = 0.0;
                for (const Leg& leg : entry.legs) {
                    if (const std::optional<model::Option> option = portfolio->option(leg.symbol)) {
                        delta += leg.ratio * option->delta();
                        vega += leg.ratio * option->vega();
                    }
                }
                const std::optional<model::Chain> first =
                    portfolio->chain(entry.legs.front().chain);
                const double forward = first ? first->forward() : kNaN;
                double move = kNaN;
                if (first && forward > 0.0) {
                    const auto call = first->call(first->atm_strike());
                    const auto put = first->put(first->atm_strike());
                    if (call && put && call->bid() > 0.0 && put->bid() > 0.0) {
                        move = (call->mid() + put->mid()) / forward;
                    }
                }
                entry.row.underlying0 = underlying;
                entry.row.forward0 = forward;
                entry.row.move0 = move;
                entry.row.entry_mid = v.mid;
                entry.row.entry_quotes = v.open;
                entry.row.entry_delta = delta;
                entry.row.entry_vega = vega;
                const year_month_day expiry = entry.legs.front().id.expiry;
                for (const StructureExit& exit : st.exits) {
                    PendingExit pending{.minute = exit.minute};
                    switch (exit.at) {
                    case StructureExit::At::kExpiry:
                        pending.date = expiry;
                        pending.at_expiry = true;
                        break;
                    case StructureExit::At::kDaysAfter:
                        pending.date = trading_days_after(calendar, today, exit.days);
                        break;
                    case StructureExit::At::kDaysBeforeExpiry:
                        pending.date = trading_days_before(calendar, expiry, exit.days);
                        break;
                    }
                    entry.exits.push_back(pending);
                }
                open.push_back(std::move(entry));
                const std::size_t e = open.size() - 1;
                for (std::size_t x = 0; x < open[e].exits.size(); ++x) {
                    PendingExit& pending = open[e].exits[x];
                    if (pending.date > today) {
                        continue;
                    }
                    // Today: an exit decided after the entry was; else it is not reached.
                    const std::optional<std::size_t> later =
                        pending.date == today ? exit_bar(pending) : std::nullopt;
                    if (later && *later > bar &&
                        (pending.at_expiry || pending.minute > open[e].row.entry_minute)) {
                        work[*later].exits.emplace_back(e, x);
                    } else {
                        not_reached(pending);
                    }
                }
            }
            for (const std::size_t s : w.picks) {
                std::optional<std::vector<Leg>> legs =
                    pick_legs(*portfolio, spec.structures[s], entry_chain[s]);
                if (!legs) {
                    continue;
                }
                waiting.push_back({.structure = s,
                                   .row = {.structure = static_cast<int>(s),
                                           .entry_day = yyyymmdd(today),
                                           .entry_minute = minute},
                                   .legs = std::move(*legs)});
                work[bar + 1].fills.push_back(waiting.size() - 1);
            }
        }
        for (Entry& e : open) { // an exit today whose minute had no bar
            for (PendingExit& x : e.exits) {
                if (!x.done && x.date == today) {
                    not_reached(x);
                }
            }
        }
        flush();
    }
    for (Entry& e : open) {
        for (PendingExit& x : e.exits) {
            if (!x.done) {
                not_reached(x);
            }
        }
    }
    flush();
    return errors;
}

// ---- Dealers' gamma

auto GammaTable::write(const std::string& path) const -> std::expected<void, std::string> {
    const auto of = [&](auto member) {
        std::vector<std::remove_cvref_t<decltype(rows_.front().*member)>> v;
        v.reserve(rows_.size());
        for (const Row& r : rows_) {
            v.push_back(r.*member);
        }
        return v;
    };
    using arrow::field;
    const auto schema = arrow::schema(
        {field("day", arrow::int32()), field("minute", arrow::int32()),
         field("spot", arrow::float64()), field("gex", arrow::float64()),
         field("gex_0dte", arrow::float64()), field("gex_calls", arrow::float64()),
         field("gex_puts", arrow::float64()), field("oi_calls", arrow::float64()),
         field("oi_puts", arrow::float64()), field("peak_strike", arrow::float64()),
         field("gex_flow", arrow::float64()), field("gex_flow_0dte", arrow::float64()),
         field("flow_contracts", arrow::float64())});
    const auto table = arrow::Table::Make(
        schema, {column(of(&Row::day)),
                 column(of(&Row::minute)),
                 column(of(&Row::spot)),
                 column(of(&Row::gex)),
                 column(of(&Row::gex_0dte)),
                 column(of(&Row::gex_calls)),
                 column(of(&Row::gex_puts)),
                 column(of(&Row::oi_calls)),
                 column(of(&Row::oi_puts)),
                 column(of(&Row::peak_strike)),
                 column(of(&Row::gex_flow)),
                 column(of(&Row::gex_flow_0dte)),
                 column(of(&Row::flow_contracts))});
    return write_table(path, table);
}

namespace {

/** Customers' positions by contract (contract_key). */
using Positions = std::unordered_map<std::uint64_t, double>;

/** One day's rows of measure_gamma, or why it could not be measured; positions: the customers'
 * before the day (none: no gamma_flow). */
auto gamma_of_day(const GammaSpec& spec, const std::string& file,
                  std::shared_ptr<const Positions> positions)
    -> std::expected<std::vector<GammaTable::Row>, std::string> {
    const adapters::LoadedDay day = adapters::load_day(file, spec.risk_free_rate,
                                                       spec.iv_price_mode, spec.cache_dir,
                                                       /*store_chains=*/false);
    if (!day->has_data()) {
        return std::unexpected(day->load_error());
    }
    const year_month_day today{std::chrono::floor<std::chrono::days>(day->day())};
    if (spec.holdout != 0 && yyyymmdd(today) >= spec.holdout) {
        return std::unexpected("in the holdout, not read");
    }
    const auto open_interest = read_open_interest(file);
    if (!open_interest) {
        return std::unexpected(open_interest.error());
    }
    adapters::Replay replay;
    replay.roll(*day);
    const model::Portfolio* portfolio = replay.portfolios().find_portfolio(day->portfolio_name());
    if (portfolio == nullptr) {
        return std::unexpected("no portfolio " + day->portfolio_name());
    }
    const model::Listing& listing = *day->listing();
    std::vector<std::uint32_t> chains; // within max_dte
    std::vector<bool> expiring;
    std::vector<int> expiry_of; // YYYYMMDD
    for (std::uint32_t c = 0; c < listing.chains().size(); ++c) {
        const auto expiry = std::chrono::floor<std::chrono::days>(listing.chains()[c].expiry);
        const auto dte = (expiry - std::chrono::sys_days{today}).count();
        if (dte >= 0 && dte <= spec.max_dte) {
            chains.push_back(c);
            expiring.push_back(dte == 0);
            expiry_of.push_back(yyyymmdd(year_month_day{expiry}));
        }
    }
    std::vector<std::string> symbols;
    for (const std::uint32_t c : chains) {
        symbols.push_back(listing.chains()[c].symbol);
    }
    const DayBars bars(*day, portfolio->calendar(), today);
    std::vector<std::size_t> priced;
    for (const int minute : spec.minutes) {
        if (const std::optional<std::size_t> bar = bars.exactly(minute)) {
            priced.push_back(*bar);
        }
    }
    std::ranges::sort(priced);
    day->set_priced_minutes(priced);
    replay.load(*day, symbols);

    std::vector<GammaTable::Row> rows;
    for (const int minute : spec.minutes) {
        const std::optional<std::size_t> bar = bars.exactly(minute);
        if (!bar) {
            continue;
        }
        replay.apply(day, *bar);
        const double spot = underlying_mid(*portfolio);
        if (!(spot > 0.0)) {
            continue;
        }
        GammaTable::Row row{.day = yyyymmdd(today), .minute = minute, .spot = spot};
        std::map<double, double> by_strike; // open interest x gamma, near the spot
        for (std::size_t i = 0; i < chains.size(); ++i) {
            const model::Listing::Chain& chain = listing.chains()[chains[i]];
            for (std::uint32_t k = chain.first; k < chain.first + chain.count; ++k) {
                const model::Contract& contract = listing.contract(k);
                const bool call = listing.type(k) == model::OptionType::CALL;
                const auto oi = open_interest->find(contract.name);
                const bool held = oi != open_interest->end() && oi->second > 0.0;
                double customers = 0.0;
                if (positions) {
                    const auto p = positions->find(contract_key(
                        expiry_of[i],
                        static_cast<std::int32_t>(std::lround(listing.strike(k) * 1000.0)), call));
                    customers = p == positions->end() ? 0.0 : p->second;
                }
                if (!held && customers == 0.0) {
                    continue;
                }
                const std::optional<model::Option> option = portfolio->option(contract.symbol);
                const double gamma = option ? option->gamma() : kNaN;
                if (!std::isfinite(gamma)) {
                    continue;
                }
                const double per = gamma * spot * spot * 0.01; // a contract's gamma: a share's
                const double dealers = -customers * per;
                row.gex_flow += dealers;
                row.flow_contracts += std::abs(customers);
                if (expiring[i]) {
                    row.gex_flow_0dte += dealers;
                }
                if (!held) {
                    continue;
                }
                const double g = oi->second * per;
                (call ? row.gex_calls : row.gex_puts) += call ? g : -g;
                (call ? row.oi_calls : row.oi_puts) += oi->second;
                if (expiring[i]) {
                    row.gex_0dte += call ? g : -g;
                }
                if (std::abs(listing.strike(k) / spot - 1.0) <= 0.02) {
                    by_strike[listing.strike(k)] += g;
                }
            }
        }
        row.gex = row.gex_calls + row.gex_puts;
        if (!by_strike.empty()) {
            row.peak_strike = std::ranges::max_element(by_strike, {}, [](const auto& kv) {
                                  return kv.second;
                              })->first;
        }
        rows.push_back(row);
    }
    return rows;
}

} // namespace

auto measure_gamma(const GammaSpec& spec, GammaTable& table)
    -> std::expected<std::vector<std::string>, std::string> {
    if (spec.minutes.empty() || spec.max_dte < 0) {
        return std::unexpected("measure_gamma needs minutes and a max_dte of 0 or more");
    }
    // The customers' positions rolled forward a day at a time, in date order (the days are), a
    // snapshot of the contracts not yet expired handed to each day's thread.
    std::vector<ContractNet> nets;
    if (!spec.contract_nets.empty()) {
        auto read = read_contract_nets(spec.contract_nets);
        if (!read) {
            return std::unexpected(read.error());
        }
        nets = std::move(*read);
        std::ranges::stable_sort(nets, {}, &ContractNet::day);
    }
    auto rolled = nets.begin();
    Positions running;
    const auto positions_before = [&](const std::string& file) -> std::shared_ptr<const Positions> {
        if (spec.contract_nets.empty()) {
            return nullptr;
        }
        const int day = std::stoi(std::filesystem::path(file).stem().string());
        for (; rolled != nets.end() && rolled->day < day; ++rolled) {
            running[contract_key(rolled->expiry, rolled->strike_milli, rolled->call)] += rolled->net;
        }
        std::erase_if(running, [&](const auto& kv) {
            return key_expiry(kv.first) < day || kv.second == 0.0;
        });
        return std::make_shared<const Positions>(running);
    };
    std::vector<std::string> errors;
    std::deque<std::future<std::expected<std::vector<GammaTable::Row>, std::string>>> measuring;
    std::size_t queued = 0;
    for (const std::string& file : spec.days) {
        for (; queued < spec.days.size() &&
               measuring.size() < std::max<std::size_t>(spec.days_ahead, 1);
             ++queued) {
            measuring.push_back(std::async(std::launch::async, gamma_of_day, std::cref(spec),
                                           std::cref(spec.days[queued]),
                                           positions_before(spec.days[queued])));
        }
        auto rows = measuring.front().get();
        measuring.pop_front();
        if (!rows) {
            errors.push_back(file + ": " + rows.error());
            continue;
        }
        for (const GammaTable::Row& row : *rows) {
            table.add(row);
        }
    }
    return errors;
}

// ---- The underlying at chosen minutes

auto read_underlying(const std::vector<std::string>& days, const std::vector<int>& minutes,
                     const std::string& cache_dir, std::vector<UnderlyingRow>& rows)
    -> std::expected<std::vector<std::string>, std::string> {
    std::vector<std::string> errors;
    for (const std::string& file : days) {
        const adapters::LoadedDay day = adapters::load_day(
            file, adapters::HistoricalData::kRiskFreeRate, adapters::HistoricalData::kIvPriceMode,
            cache_dir, /*store_chains=*/false);
        if (!day->has_data()) {
            errors.push_back(file + ": " + day->load_error());
            continue;
        }
        model::Portfolios portfolios;
        day->build_portfolio(portfolios);
        const model::Portfolio* portfolio = portfolios.find_portfolio(day->portfolio_name());
        if (portfolio == nullptr) {
            errors.push_back(file + ": no portfolio " + day->portfolio_name());
            continue;
        }
        const year_month_day today{std::chrono::floor<std::chrono::days>(day->day())};
        const DayBars bars(*day, portfolio->calendar(), today);
        for (const int minute : minutes) {
            if (const std::optional<std::size_t> bar = bars.exactly(minute)) {
                rows.push_back({.day = yyyymmdd(today),
                                .minute = minute,
                                .bid = day->underlying_bid(*bar),
                                .ask = day->underlying_ask(*bar)});
            }
        }
    }
    return errors;
}

auto write_underlying(const std::string& path, const std::vector<UnderlyingRow>& rows)
    -> std::expected<void, std::string> {
    std::vector<int> day, minute;
    std::vector<double> bid, ask;
    for (const UnderlyingRow& r : rows) {
        day.push_back(r.day);
        minute.push_back(r.minute);
        bid.push_back(r.bid);
        ask.push_back(r.ask);
    }
    using arrow::field;
    const auto table = arrow::Table::Make(
        arrow::schema({field("day", arrow::int32()), field("minute", arrow::int32()),
                       field("bid", arrow::float64()), field("ask", arrow::float64())}),
        {column(day), column(minute),
         column(bid), column(ask)});
    return write_table(path, table);
}

} // namespace research
