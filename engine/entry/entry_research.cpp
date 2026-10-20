// entry_research: research tools on the offline data (engine/research). A subcommand and a JSON
// spec; one JSON line a job on stdout, then {"status": "ok"}. A spec that reads a file named for a
// day in its portfolio's holdout (config.json, research::Holdouts) is refused unless it says
// "reach_holdout": true; structures and gamma also refuse a day whose quotes are in it.
//
//   entry_research fills <spec.json>
//     Limit orders simulated on the NBBO ticks and the trades (research/fills.hpp). The spec:
//     {"t0_minutes": [minutes after 09:30], "latency_ms": 1000, "windows_ms": [...],
//      "fill_positions": [...], "tick_size": 0.01,
//      "jobs": [{"day": "YYYY-MM-DD", "expiry": "YYYY-MM-DD", "ticks": <file>, "trades": <file>,
//                "strike_min": .., "strike_max": .., "out": <parquet>}]}
//     A job is one day and expiry; its contracts are read one at a time.
//
//   entry_research structures <spec.json>
//     Option structures opened and valued on the backtest days (research/structures.hpp):
//     {"days": [<day files of one portfolio, in date order>],
//      "cache_dir": <the day cache>, "store_chains": false (only day indexes stored there),
//      "days_ahead": 4 (days loaded ahead, each on a thread),
//      "risk_free_rate": 0.05, "iv_price_mode": "mid",
//      "structures": [{"name": .., "entry_minutes": [..],
//        "legs": [{"right": "C" | "P", "ratio": n,
//                  "strike": {"pct": x} | {"moves": x} | {"delta": x} | {"leg": k (1-based)},
//                  "expiry": {"first_at_least": n} | {"exactly": n}
//                            | {"nearest": n, "min": a, "max": b} | {"leg": k (1-based)}}],
//        "exits": [{"label": .., "at": "expiry"} | {"label": .., "days_after": n, "minute": m}
//                  | {"label": .., "days_before_expiry": n, "minute": m}]}],
//      "out": <parquet>}
//
//   entry_research trade_places|combo_places <spec.json>
//     Where the trades print in the NBBO before them, summed in cells (research/fills.hpp):
//     {"days": [{"day": "YYYY-MM-DD", "trades": <file>, "backtest": <its backtest day: the
//                underlying, trade_places only>}],
//      "cache_dir": <the day cache>, "threads": 3,
//      "keys": [{"name": .., "measure": .., "edges": [..], "labels": [..]}], "out": <parquet>}
//     A day a thread at a time, its trades read a row group at a time.
//
//   entry_research flow <spec.json>
//     The order flow, trades signed by the NBBO before them, with their Greeks (research/fills.hpp
//     FlowTable):
//     {"days": [{"day": "YYYY-MM-DD", "trades": <file>, "backtest": <its backtest day>}],
//      "cache_dir": .., "threads": 3, "big_size": 1000, "out": <parquet>, "big_out": <parquet>,
//      "contracts_out": <parquet: each contract's net and volume a day, optional>}
//
//   entry_research gamma <spec.json>
//     The dealers' gamma by the day's open interest (research/structures.hpp):
//     {"days": [<day files of one portfolio>], "cache_dir": ..,
//      "minutes": [minutes since the open], "max_dte": 45, "days_ahead": 4, "out": <parquet>,
//      "contract_nets": <flow's contracts_out: the dealers' gamma by the customers' positions too>}
//
//   entry_research underlying <spec.json>
//     The underlying's bid and ask at chosen minutes of each day, from the days' indexes:
//     {"days": [<day files>], "minutes": [minutes since the open], "cache_dir": .., "out":
//     <parquet>}
//
//   entry_research intraminute <spec.json>
//     The NBBO of every contract at four times of every minute (research/fills.hpp):
//     {"jobs": [{"day": "YYYY-MM-DD", "expiry": "YYYY-MM-DD", "ticks": <file>}], "threads": 2,
//      "out": <parquet>}

#include "research/fills.hpp"
#include "research/offline_data.hpp"
#include "research/structures.hpp"

#include <atomic>
#include <chrono>
#include <format>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <thread>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace {

using nlohmann::json;

using research::parse_day;
using research::yyyymmdd;

// The holdout of each portfolio (config.json), or none when the spec says "reach_holdout": true.
research::Holdouts holdouts;

/** The portfolio of a day file: .../<PORTFOLIO>/<PORTFOLIO>-YYYY-MM/<day>.parquet. */
auto portfolio_of(const std::string& file) -> std::string {
    return std::filesystem::path(file).parent_path().parent_path().filename().string();
}

/** The files a spec reads: every string ending .parquet but the outputs'. */
void spec_files(const json& j, const std::string& key, std::vector<std::string>& out) {
    if (j.is_string()) {
        const std::string& s = j.get_ref<const std::string&>();
        if (s.ends_with(".parquet") && !key.ends_with("out")) {
            out.push_back(s);
        }
    } else if (j.is_array()) {
        for (const json& x : j) {
            spec_files(x, key, out);
        }
    } else if (j.is_object()) {
        for (const auto& [k, x] : j.items()) {
            spec_files(x, k, out);
        }
    }
}

auto ny_midnight_ms(std::chrono::year_month_day day) -> std::int64_t {
    const std::chrono::zoned_time midnight{"America/New_York", std::chrono::local_days{day}};
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               midnight.get_sys_time().time_since_epoch())
        .count();
}


/** The command's last lines: its line with the seconds it took, then {"status": "ok"}. */
auto finish(json line, std::chrono::steady_clock::time_point started) -> int {
    line["seconds"] =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    std::cout << line.dump() << '\n' << json{{"status", "ok"}}.dump() << '\n';
    return 0;
}

/** fn(i, thread) for each i below n, on `threads` threads, each taking the next i when free. */
void parallel_for(std::size_t n, std::size_t threads,
                  const std::function<void(std::size_t, std::size_t)>& fn) {
    std::atomic<std::size_t> next{0};
    std::vector<std::jthread> workers;
    for (std::size_t t = 0; t < std::max<std::size_t>(threads, 1); ++t) {
        workers.emplace_back([&, t] {
            for (std::size_t i = next++; i < n; i = next++) {
                fn(i, t);
            }
        });
    }
}

auto run_fills(const json& spec) -> int {
    const auto t0_minutes = spec.at("t0_minutes").get<std::vector<int>>();
    const auto windows = spec.at("windows_ms").get<std::vector<std::int64_t>>();
    const auto positions = spec.at("fill_positions").get<std::vector<double>>();
    const std::int64_t latency = spec.value("latency_ms", std::int64_t{1000});
    const double tick = spec.value("tick_size", 0.01);
    for (const json& job : spec.at("jobs")) {
        const auto started = std::chrono::steady_clock::now();
        const auto day = parse_day(job.at("day").get<std::string>());
        const auto expiry = parse_day(job.at("expiry").get<std::string>());
        const std::int64_t midnight = ny_midnight_ms(day);
        std::vector<std::int64_t> t0s;
        for (const int m : t0_minutes) {
            t0s.push_back(midnight + (((9 * 60) + 30 + m) * 60'000LL) + latency);
        }
        auto trades = research::read_trades(job.at("trades").get<std::string>(), yyyymmdd(expiry),
                                            midnight);
        if (!trades) {
            std::cout << json{{"day", job.at("day")}, {"error", trades.error()}}.dump() << '\n';
            continue;
        }
        const double lo = job.at("strike_min").get<double>();
        const double hi = job.at("strike_max").get<double>();
        const int dte = static_cast<int>(
            (std::chrono::sys_days{expiry} - std::chrono::sys_days{day}).count());
        const research::LimitOrders orders{
            .t0_ms = t0s, .window_ms = windows, .fill_positions = positions, .tick_size = tick};
        research::FillTable table;
        int contracts = 0;
        const research::TradeSeries none;
        auto read = research::for_each_contract(
            job.at("ticks").get<std::string>(), [&](double k) { return k >= lo && k <= hi; },
            [&](const research::Contract& c, research::QuoteSeries&& quotes) {
                ++contracts;
                const auto it = trades->find(c);
                table.add(c, yyyymmdd(day), dte,
                          research::simulate_limits(quotes, it == trades->end() ? none : it->second,
                                                    orders));
            });
        json line{{"day", job.at("day")}, {"expiry", job.at("expiry")}, {"contracts", contracts},
                  {"orders", table.rows()}};
        if (!read) {
            line["error"] = read.error();
        } else if (auto written = table.write(job.at("out").get<std::string>()); !written) {
            line["error"] = written.error();
        }
        line["seconds"] =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        std::cout << line.dump() << std::endl;
    }
    std::cout << json{{"status", "ok"}}.dump() << '\n';
    return 0;
}

auto leg_of(const json& j) -> research::StructureLeg {
    using model::LegRule;
    LegRule rule{.ratio = j.at("ratio").get<int>(),
                 .right = j.at("right").get<std::string>() == "P" ? model::OptionType::PUT
                                                                  : model::OptionType::CALL};
    research::StructureLeg leg;
    const json& strike = j.at("strike");
    if (strike.contains("leg")) {
        leg.strike_of = strike.at("leg").get<int>() - 1;
    } else if (strike.contains("moves")) {
        rule.strike_by = LegRule::StrikeBy::kImpliedMoves;
        rule.strike = strike.at("moves").get<double>();
    } else if (strike.contains("delta")) {
        rule.strike_by = LegRule::StrikeBy::kDelta;
        rule.strike = strike.at("delta").get<double>();
    } else {
        rule.strike = strike.at("pct").get<double>();
    }
    const json& expiry = j.at("expiry");
    if (expiry.contains("leg")) {
        leg.expiry_of = expiry.at("leg").get<int>() - 1;
    } else if (expiry.contains("exactly")) {
        rule.expiry_by = LegRule::ExpiryBy::kExactly;
        rule.dte = expiry.at("exactly").get<int>();
    } else if (expiry.contains("nearest")) {
        rule.expiry_by = LegRule::ExpiryBy::kNearest;
        rule.dte = expiry.at("nearest").get<int>();
        rule.min_dte = expiry.at("min").get<int>();
        rule.max_dte = expiry.at("max").get<int>();
    } else {
        rule.dte = expiry.at("first_at_least").get<int>();
    }
    leg.rule = rule;
    return leg;
}

auto run_structures(const json& spec) -> int {
    const auto started = std::chrono::steady_clock::now();
    research::ScreenSpec screen{.days = spec.at("days").get<std::vector<std::string>>(),
                                .cache_dir = spec.value("cache_dir", std::string()),
                                .store_chains = spec.value("store_chains", false),
                                .days_ahead = spec.value("days_ahead", std::size_t{4}),
                                .risk_free_rate = spec.value("risk_free_rate", 0.05),
                                .iv_price_mode = spec.value("iv_price_mode", std::string("mid"))};
    screen.holdout = screen.days.empty() ? 0 : holdouts.of(portfolio_of(screen.days.front()));
    for (const json& s : spec.at("structures")) {
        research::Structure structure{.name = s.at("name").get<std::string>(),
                                      .entry_minutes =
                                          s.at("entry_minutes").get<std::vector<int>>()};
        for (const json& leg : s.at("legs")) {
            structure.legs.push_back(leg_of(leg));
        }
        for (const json& x : s.at("exits")) {
            research::StructureExit exit{.label = x.at("label").get<std::string>()};
            if (x.contains("days_after")) {
                exit.at = research::StructureExit::At::kDaysAfter;
                exit.days = x.at("days_after").get<int>();
                exit.minute = x.at("minute").get<int>();
            } else if (x.contains("days_before_expiry")) {
                exit.at = research::StructureExit::At::kDaysBeforeExpiry;
                exit.days = x.at("days_before_expiry").get<int>();
                exit.minute = x.at("minute").get<int>();
            }
            structure.exits.push_back(exit);
        }
        screen.structures.push_back(std::move(structure));
    }
    research::StructureTable table(screen.structures);
    auto errors = research::screen_structures(screen, table);
    if (!errors) {
        std::cout << json{{"status", "error"}, {"error", errors.error()}}.dump() << '\n';
        return 1;
    }
    json line{{"days", screen.days.size()}, {"rows", table.rows()}, {"errors", *errors}};
    if (auto written = table.write(spec.at("out").get<std::string>()); !written) {
        line["error"] = written.error();
    }
    return finish(std::move(line), started);
}

/** The underlying's mid a minute (minutes since New York midnight), from the backtest day's index;
 * the load error, empty when none. */
auto underlying_mids(const std::string& backtest, std::chrono::year_month_day day,
                     const std::string& cache_dir, std::vector<int>& minutes,
                     std::vector<double>& underlying) -> std::string {
    const adapters::LoadedDay u =
        adapters::load_day(backtest, adapters::HistoricalData::kRiskFreeRate,
                           adapters::HistoricalData::kIvPriceMode, cache_dir,
                           /*store_chains=*/false);
    if (!u->has_data()) {
        return u->load_error();
    }
    const std::int64_t midnight = ny_midnight_ms(day);
    for (std::size_t i = 0; i < u->minutes(); ++i) {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            u->time(i).time_since_epoch()).count();
        minutes.push_back(static_cast<int>((ms - midnight) / 60'000));
        underlying.push_back((u->underlying_bid(i) + u->underlying_ask(i)) / 2.0);
    }
    return {};
}

/** Calls run(i, table) for each i below n on `threads` threads, each with its own table (made by
 * make), then merges them into the first; a line every 100 done. */
template <typename Table>
auto in_threads(std::size_t n, std::size_t threads, const std::function<Table()>& make,
                const std::function<void(std::size_t, Table&)>& run) -> Table {
    std::vector<Table> tables;
    for (std::size_t t = 0; t < std::max<std::size_t>(threads, 1); ++t) {
        tables.push_back(make());
    }
    std::atomic<std::size_t> done{0};
    std::mutex out;
    parallel_for(n, tables.size(), [&](std::size_t i, std::size_t t) {
        run(i, tables[t]);
        if (const std::size_t d = ++done; d % 100 == 0) {
            const std::lock_guard lock(out);
            std::cout << json{{"done", d}, {"of", n}}.dump() << std::endl;
        }
    });
    for (std::size_t t = 1; t < tables.size(); ++t) {
        tables[0].merge(std::move(tables[t]));
    }
    return std::move(tables[0]);
}

auto keys_of(const json& spec) -> std::vector<research::CellKey> {
    std::vector<research::CellKey> keys;
    for (const json& k : spec.at("keys")) {
        keys.push_back({.name = k.at("name").get<std::string>(),
                        .measure = k.value("measure", std::string()),
                        .edges = k.value("edges", std::vector<double>{}),
                        .labels = k.value("labels", std::vector<std::string>{})});
    }
    return keys;
}

auto run_places(const std::string& command, const json& spec) -> int {
    const auto started = std::chrono::steady_clock::now();
    const bool combos = command == "combo_places";
    const std::vector<research::CellKey> keys = keys_of(spec);
    const auto make = [&] {
        return combos ? research::combo_cells(keys) : research::trade_cells(keys);
    };
    (void)make(); // the keys checked before any day is read
    const json& days = spec.at("days");
    const std::string cache_dir = spec.value("cache_dir", std::string());
    std::mutex lock;
    std::vector<std::string> errors;
    std::atomic<std::size_t> trades{0};
    const research::TradeCells cells = in_threads<research::TradeCells>(
        days.size(), spec.value("threads", std::size_t{3}), make,
        [&](std::size_t d, research::TradeCells& into) {
            const json& job = days[d];
            const auto day = parse_day(job.at("day").get<std::string>());
            const auto fail = [&](const std::string& why) {
                const std::lock_guard guard(lock);
                errors.push_back(job.at("day").get<std::string>() + ": " + why);
            };
            std::vector<int> minutes;
            std::vector<double> underlying;
            if (!combos) {
                if (auto error = underlying_mids(job.at("backtest").get<std::string>(), day,
                                                 cache_dir, minutes, underlying);
                    !error.empty()) {
                    return fail(error);
                }
            }
            const auto rows = research::read_trade_rows(
                job.at("trades").get<std::string>(),
                combos ? std::span<const std::int16_t>(research::kComboConditions)
                       : std::span<const std::int16_t>{});
            if (!rows) {
                return fail(rows.error());
            }
            trades += rows->rows();
            if (combos) {
                research::place_combos(*rows, day, into);
            } else {
                research::place_trades(*rows, day, minutes, underlying, into);
            }
        });
    json line{{"days", days.size()}, {"trades", trades.load()}, {"cells", cells.cells()},
              {"errors", errors}};
    if (auto written = cells.write(spec.at("out").get<std::string>()); !written) {
        line["error"] = written.error();
    }
    return finish(std::move(line), started);
}

auto run_flow(const json& spec) -> int {
    const auto started = std::chrono::steady_clock::now();
    const json& days = spec.at("days");
    const std::string cache_dir = spec.value("cache_dir", std::string());
    const int big_size = spec.value("big_size", 1000);
    std::vector<std::string> errors;
    std::atomic<std::size_t> trades{0};
    std::mutex lock;
    const research::FlowTable table = in_threads<research::FlowTable>(
        days.size(), spec.value("threads", std::size_t{3}),
        [&] { return research::FlowTable(big_size); },
        [&](std::size_t d, research::FlowTable& into) {
            const json& job = days[d];
            const auto day = parse_day(job.at("day").get<std::string>());
            std::vector<int> minutes;
            std::vector<double> underlying;
            std::string error = underlying_mids(job.at("backtest").get<std::string>(), day,
                                                cache_dir, minutes, underlying);
            if (error.empty()) {
                const auto rows = research::read_trade_rows(job.at("trades").get<std::string>());
                if (rows) {
                    trades += rows->rows();
                    into.add(*rows, day, minutes, underlying);
                } else {
                    error = rows.error();
                }
            }
            if (!error.empty()) {
                const std::lock_guard guard(lock);
                errors.push_back(job.at("day").get<std::string>() + ": " + error);
            }
        });
    json line{{"days", days.size()},
              {"trades", trades.load()},
              {"rows", table.rows()},
              {"big", table.big_rows()},
              {"errors", errors}};
    if (auto written = table.write(spec.at("out").get<std::string>()); !written) {
        line["error"] = written.error();
    }
    if (auto written = table.write_big(spec.at("big_out").get<std::string>()); !written) {
        line["error"] = written.error();
    }
    if (spec.contains("contracts_out")) {
        if (auto written = table.write_contracts(spec.at("contracts_out").get<std::string>());
            !written) {
            line["error"] = written.error();
        }
    }
    return finish(std::move(line), started);
}

auto run_intraminute(const json& spec) -> int {
    const auto started = std::chrono::steady_clock::now();
    const json& jobs = spec.at("jobs");
    std::vector<std::pair<std::chrono::year_month_day, int>> days(jobs.size());
    std::vector<research::IntraminuteRows> rows(jobs.size());
    std::vector<std::string> errors(jobs.size());
    std::mutex out;
    const std::size_t threads = spec.value("threads", std::size_t{2});
    parallel_for(jobs.size(), threads, [&](std::size_t j, std::size_t) {
        const auto day = parse_day(jobs[j].at("day").get<std::string>());
        const auto expiry = parse_day(jobs[j].at("expiry").get<std::string>());
        days[j] = {day, static_cast<int>((std::chrono::sys_days{expiry} -
                                          std::chrono::sys_days{day}).count())};
        const std::int64_t first = ny_midnight_ms(day) + ((9 * 60) + 31) * 60'000LL;
        std::vector<std::int64_t> minutes(389);
        for (std::size_t m = 0; m < minutes.size(); ++m) {
            minutes[m] = first + static_cast<std::int64_t>(m) * 60'000;
        }
        const auto read = research::for_each_contract(
            jobs[j].at("ticks").get<std::string>(), [](double) { return true; },
            [&](const research::Contract& c, research::QuoteSeries&& quotes) {
                const auto seed = std::hash<std::string>{}(
                    std::format("{}{}{}", yyyymmdd(day), c.strike, c.call));
                research::sample_intraminute(quotes, minutes, seed, rows[j]);
            });
        if (!read) {
            errors[j] = read.error();
        }
        const std::lock_guard lock(out);
        std::cout
            << json{{"day", jobs[j].at("day")}, {"rows", rows[j].minute.size()}}.dump()
            << std::endl;
    });
    json line{{"jobs", jobs.size()}};
    std::erase(errors, std::string());
    line["errors"] = errors;
    if (auto written = research::write_intraminute(spec.at("out").get<std::string>(), days, rows);
        !written) {
        line["error"] = written.error();
    }
    return finish(std::move(line), started);
}

auto run_gamma(const json& spec) -> int {
    const auto started = std::chrono::steady_clock::now();
    research::GammaSpec gamma{.days = spec.at("days").get<std::vector<std::string>>(),
                              .cache_dir = spec.value("cache_dir", std::string()),
                              .minutes = spec.value("minutes", std::vector<int>{15}),
                              .max_dte = spec.value("max_dte", 45),
                              .days_ahead = spec.value("days_ahead", std::size_t{4}),
                              .contract_nets = spec.value("contract_nets", std::string())};
    gamma.holdout = gamma.days.empty() ? 0 : holdouts.of(portfolio_of(gamma.days.front()));
    research::GammaTable table;
    auto errors = research::measure_gamma(gamma, table);
    if (!errors) {
        std::cout << json{{"status", "error"}, {"error", errors.error()}}.dump() << '\n';
        return 1;
    }
    json line{{"days", gamma.days.size()}, {"rows", table.rows()}, {"errors", *errors}};
    if (auto written = table.write(spec.at("out").get<std::string>()); !written) {
        line["error"] = written.error();
    }
    return finish(std::move(line), started);
}

auto run_underlying(const json& spec) -> int {
    const auto started = std::chrono::steady_clock::now();
    const auto days = spec.at("days").get<std::vector<std::string>>();
    std::vector<research::UnderlyingRow> rows;
    auto errors = research::read_underlying(days, spec.at("minutes").get<std::vector<int>>(),
                                            spec.value("cache_dir", std::string()), rows);
    json line{{"days", days.size()}, {"rows", rows.size()}, {"errors", *errors}};
    if (auto written = research::write_underlying(spec.at("out").get<std::string>(), rows);
        !written) {
        line["error"] = written.error();
    }
    return finish(std::move(line), started);
}

} // namespace

int main(int argc, char** argv) {
    const std::map<std::string, std::function<int(const json&)>> commands{
        {"fills", run_fills},
        {"structures", run_structures},
        {"trade_places", [](const json& spec) { return run_places("trade_places", spec); }},
        {"combo_places", [](const json& spec) { return run_places("combo_places", spec); }},
        {"flow", run_flow},
        {"intraminute", run_intraminute},
        {"gamma", run_gamma},
        {"underlying", run_underlying}};
    const auto command = argc == 3 ? commands.find(argv[1]) : commands.end();
    if (command == commands.end()) {
        std::cerr << "usage: entry_research <command> <spec.json>; the commands:";
        for (const auto& [name, _] : commands) {
            std::cerr << ' ' << name;
        }
        std::cerr << '\n';
        return 2;
    }
    try {
        std::ifstream in(argv[2]);
        if (!in) {
            std::cerr << "cannot read " << argv[2] << '\n';
            return 2;
        }
        const json spec = json::parse(in);
        if (spec.contains("holdout")) {
            throw std::invalid_argument("the holdout is read from config.json, not the spec; "
                                        "\"reach_holdout\": true reads it");
        }
        if (!spec.value("reach_holdout", false)) {
            holdouts = research::Holdouts::load();
            std::vector<std::string> files;
            spec_files(spec, "", files);
            if (const auto held = holdouts.first_held(files)) {
                throw std::invalid_argument(std::format(
                    "{} is in {}'s holdout (from {}): \"reach_holdout\": true reads it", *held,
                    portfolio_of(*held), holdouts.of(portfolio_of(*held))));
            }
        }
        return command->second(spec);
    } catch (const std::exception& e) {
        std::cout << json{{"status", "error"}, {"error", e.what()}}.dump() << '\n';
        return 1;
    }
}
