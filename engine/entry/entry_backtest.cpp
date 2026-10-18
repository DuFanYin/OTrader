#include "core/strategy/strategy_engine.hpp"
#include "adapters/market_data/historical_data.hpp"
#include "assembly/backtest.hpp"
#include "model/daily_table.hpp"
#include "model/utility.hpp"
#include "strategy/strategy_registry.hpp"
#include "utilities/log_level.hpp"

#include <algorithm>
#include <nlohmann/json.hpp>
#include <chrono>
#include <cmath>
#include <fstream>
#include <format>
#include <iostream>
#include <map>
#include <optional>
#include <malloc.h>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unistd.h>
#include <vector>

namespace {

// Largest-Triangle-Three-Buckets downsampling of y (x = index); the selected indices, first and
// last included, or all of them when N <= threshold.
std::vector<size_t> lttb_downsample_indices(const std::vector<double>& y, size_t threshold) {
    const size_t N = y.size();
    if (threshold >= N || threshold < 3 || N == 0) {
        std::vector<size_t> idx(N);
        for (size_t i = 0; i < N; ++i) {
            idx[i] = i;
        }
        return idx;
    }

    std::vector<size_t> out;
    out.reserve(threshold);
    out.push_back(0);

    const double bucket_size = static_cast<double>(N - 2) / static_cast<double>(threshold - 2);
    size_t a = 0;

    for (size_t i = 0; i < threshold - 2; ++i) {
        const double bucket_start = 1.0 + static_cast<double>(i) * bucket_size;
        const double bucket_end = bucket_start + bucket_size;

        const size_t start = static_cast<size_t>(std::floor(bucket_start));
        const size_t end = std::min(static_cast<size_t>(std::floor(bucket_end)), N - 1);

        // Point C: the next bucket's average.
        const double next_bucket_start = bucket_end;
        const double next_bucket_end = next_bucket_start + bucket_size;
        const size_t next_start = static_cast<size_t>(std::floor(next_bucket_start));
        const size_t next_end = std::min(static_cast<size_t>(std::floor(next_bucket_end)), N - 1);

        double avg_x = 0.0;
        double avg_y = 0.0;
        size_t avg_count = 0;
        for (size_t j = next_start; j < next_end; ++j) {
            avg_x += static_cast<double>(j);
            avg_y += y[j];
            ++avg_count;
        }
        if (avg_count == 0) {
            avg_x = static_cast<double>(a);
            avg_y = y[a];
        } else {
            avg_x /= static_cast<double>(avg_count);
            avg_y /= static_cast<double>(avg_count);
        }

        const double ax = static_cast<double>(a);
        const double ay = y[a];

        double max_area = -1.0;
        size_t selected = start;
        for (size_t j = start; j < end; ++j) {
            const double bx = static_cast<double>(j);
            const double by = y[j];
            const double area = std::abs((ax - avg_x) * (by - ay) - (ax - bx) * (avg_y - ay));
            if (area > max_area) {
                max_area = area;
                selected = j;
            }
        }

        out.push_back(selected);
        a = selected;
    }

    out.push_back(N - 1);
    return out;
}

// The result goes to stdout and nothing else does: main moves stdout to a descriptor of its own
// (result_fd) and points fd 1 at stderr, so the logs, and anything else printed, go there.
int result_fd = STDOUT_FILENO;

/** `out` as one line. */
void print_json(const nlohmann::json& out) {
    const std::string text = out.dump() + "\n";
    std::cout << std::flush;
    for (std::size_t done = 0; done < text.size();) {
        const ssize_t n = ::write(result_fd, text.data() + done, text.size() - done);
        if (n <= 0) {
            return;
        }
        done += static_cast<std::size_t>(n);
    }
}

void print_error_json(const std::string& msg) {
    print_json({{"status", "error"}, {"error", msg}});
}

using FillModel = adapters::BacktestGateway::FillModel;

auto fill_model_of(const std::string& name) -> FillModel {
    if (name == "quote") {
        return FillModel::Quote;
    }
    if (name == "shown_size") {
        return FillModel::ShownSize;
    }
    throw std::invalid_argument("Unknown fill model '" + name + "': quote or shown_size");
}

/** A trial's costs, as the server's ledger.Costs writes them: "fee_rate", "fill_position",
 * "multi_leg_fill_position", "fill" (quote or shown_size), "delay_bars" and "risk" ({limit:
 * value}). One left out keeps the engine's default; an unknown one is refused. */
void read_costs(const nlohmann::json& costs, backtest::BacktestTrial& trial) {
    adapters::BacktestGateway::Execution& e = trial.execution;
    for (const auto& [key, value] : costs.items()) {
        if (key == "fee_rate") {
            e.fee_rate = value.get<double>();
        } else if (key == "fill_position") {
            e.fill_position = value.get<double>();
        } else if (key == "multi_leg_fill_position") {
            e.multi_leg_fill_position = value.get<double>();
        } else if (key == "fill") {
            e.fill = fill_model_of(value.get<std::string>());
        } else if (key == "delay_bars") {
            e.delay_bars = value.get<int>();
        } else if (key == "risk") {
            trial.risk_setting = value.get<std::unordered_map<std::string, double>>();
        } else {
            throw std::invalid_argument("No cost '" + key + "'");
        }
    }
}

// Every registered strategy with its parameters (default and search range), source and library;
// and the strategy libraries not loaded, with why.
void print_strategies_json() {
    nlohmann::json classes = nlohmann::json::array();
    for (const std::string& name : strategy::StrategyRegistry::get_all_strategy_class_names()) {
        nlohmann::json parameters = nlohmann::json::array();
        for (const model::Parameter& p : strategy::StrategyRegistry::parameters(name)) {
            parameters.push_back({{"name", std::string(p.name)},
                                  {"default", p.default_value},
                                  {"min", p.min},
                                  {"max", p.max},
                                  {"integer", p.integer}});
        }
        classes.push_back({{"name", name},
                           {"source", strategy::StrategyRegistry::source(name)},
                           {"library", strategy::StrategyRegistry::library(name)},
                           {"parameters", std::move(parameters)}});
    }
    nlohmann::json refused = nlohmann::json::array();
    for (const strategy::StrategyRegistry::Refusal& r : strategy::StrategyRegistry::refused()) {
        refused.push_back({{"library", r.library}, {"why", r.why}});
    }
    print_json({{"status", "ok"}, {"strategies", std::move(classes)}, {"refused", std::move(refused)}});
}

/** YYYYMMDD of the day a file holds; 0 when its name is not a date. */
auto date_number(const std::string& file) -> int {
    const std::optional<std::chrono::year_month_day> day = adapters::day_of_file(file);
    if (!day) {
        return 0;
    }
    return (static_cast<int>(day->year()) * 10000) +
           (static_cast<int>(static_cast<unsigned>(day->month())) * 100) +
           static_cast<int>(static_cast<unsigned>(day->day()));
}

auto time_text(model::DateTime t) -> std::string {
    return std::format("{:%Y-%m-%d %H:%M:%S}", std::chrono::floor<std::chrono::seconds>(t));
}

/** {name: []} for each name: a table's columns, filled a row at a time. */
auto columns(std::initializer_list<const char*> names) -> nlohmann::json {
    nlohmann::json out = nlohmann::json::object();
    for (const char* name : names) {
        out[name] = nlohmann::json::array();
    }
    return out;
}

/** P&L by source, a column each. */
struct Attribution {
    std::vector<double> delta, gamma, theta, vega, other, fills;

    void add(const model::HoldingSummary& s) {
        delta.push_back(s.pnl_delta);
        gamma.push_back(s.pnl_gamma);
        theta.push_back(s.pnl_theta);
        vega.push_back(s.pnl_vega);
        other.push_back(s.pnl_other);
        fills.push_back(s.pnl_fills);
    }
    [[nodiscard]] nlohmann::json json() const {
        return {{"delta", delta}, {"gamma", gamma}, {"theta", theta},
                {"vega", vega},   {"other", other}, {"fills", fills}};
    }
};

/** A trial's line of the output: its strategy, portfolio and run time, its errors, its days and its
 * round trips a column each (a day by its date, YYYYMMDD); its fills, its log and its minute chart
 * (P&L and Greeks, downsampled) when it asked for them. What the days add up to is the reader's. */
auto trial_line(const backtest::BacktestRunSummary& summary,
                const backtest::BacktestTrial& trial, const std::string& id)
    -> nlohmann::json {
    // Days whose file failed to load are left out: their reason is already in the errors.
    std::vector<const backtest::DailyResult*> loaded_days;
    loaded_days.reserve(summary.daily_results.size());
    for (const auto& daily : summary.daily_results) {
        if (!daily.file_path.empty() && !daily.load_failed) {
            loaded_days.push_back(&daily);
        }
    }

    // The run's bars, its days one after another, for the chart: the days carry their holding, so
    // the P&L runs on.
    std::vector<backtest::Metric> metrics;
    std::vector<double> pnl;
    std::vector<int> day_starts; // where each day after the first starts
    for (const auto* daily : loaded_days) {
        if (daily->metrics.empty()) {
            continue;
        }
        if (!metrics.empty()) {
            day_starts.push_back(static_cast<int>(metrics.size()));
        }
        metrics.append_range(daily->metrics);
        for (const backtest::Metric& m : daily->metrics) {
            pnl.push_back(m.pnl);
        }
    }

    std::vector<int> date;
    std::vector<int> orders;
    std::vector<double> day_pnl;
    std::vector<double> net_pnl;
    std::vector<double> fees;
    std::vector<double> max_margin;
    std::vector<double> max_delta;
    std::vector<double> max_gamma;
    std::vector<double> max_theta;
    std::vector<bool> flat;
    Attribution by_source;
    nlohmann::json fills = columns({"date", "time", "symbol", "order_id", "side", "price", "quantity"});
    nlohmann::json log = columns({"date", "time", "level", "source", "msg"});
    for (const auto* daily : loaded_days) {
        const int on = date_number(daily->file_path);
        date.push_back(on);
        day_pnl.push_back(daily->daily_pnl);
        net_pnl.push_back(daily->daily_pnl - daily->daily_fees);
        fees.push_back(daily->daily_fees);
        orders.push_back(daily->total_orders);
        flat.push_back(daily->flat_at_close);
        max_margin.push_back(daily->max_margin);
        max_delta.push_back(daily->max_delta);
        max_gamma.push_back(daily->max_gamma);
        max_theta.push_back(daily->max_theta);
        by_source.add(daily->summary);
        for (const model::Fill& fill : daily->fills) {
            fills["date"].push_back(on);
            fills["time"].push_back(fill.datetime != model::DateTime{} ? time_text(fill.datetime)
                                                                       : std::string{});
            fills["symbol"].push_back(fill.symbol);
            fills["order_id"].push_back(fill.order_id);
            fills["side"].push_back(model::to_string(fill.side));
            fills["price"].push_back(fill.price);
            fills["quantity"].push_back(fill.quantity);
        }
        for (const utilities::LogRecord& record : daily->log) { // at the data's time
            log["date"].push_back(on);
            log["time"].push_back(time_text(record.at));
            log["level"].push_back(utilities::level_to_string(record.level));
            log["source"].push_back(record.source);
            log["msg"].push_back(record.msg);
        }
    }

    nlohmann::json trips = columns({"opened", "closed", "days", "net", "fees", "max_loss",
                                    "entry_cost", "exit_cost", "settled", "share_fills"});
    Attribution trip_sources;
    for (const backtest::RoundTrip& trip : summary.round_trips) {
        const auto at = [](model::DateTime t) -> nlohmann::json {
            return t == model::DateTime{} ? nlohmann::json(nullptr) : nlohmann::json(time_text(t));
        };
        trips["opened"].push_back(at(trip.opened));
        trips["closed"].push_back(at(trip.closed));
        trips["days"].push_back(trip.days);
        trips["net"].push_back(trip.net);
        trips["fees"].push_back(trip.fees);
        trips["max_loss"].push_back(std::isfinite(trip.max_loss) ? nlohmann::json(trip.max_loss)
                                                                  : nlohmann::json(nullptr));
        trips["entry_cost"].push_back(trip.entry_cost);
        trips["exit_cost"].push_back(trip.exit_cost);
        trips["settled"].push_back(trip.settled);
        trips["share_fills"].push_back(trip.share_fills);
        trip_sources.add(trip.attribution);
    }
    trips["attribution"] = trip_sources.json();

    nlohmann::json out = {
        {"id", id},
        {"result",
         {{"strategy_name", summary.strategy_name},
          {"portfolio_name", summary.portfolio_name},
          {"duration_seconds", summary.duration_seconds}}},
        {"errors", summary.errors},
        {"days",
         {{"date", date},
          {"pnl", day_pnl},
          {"net_pnl", net_pnl},
          {"fees", fees},
          {"orders", orders},
          {"flat", flat},
          {"max_margin", max_margin},
          {"max_delta", max_delta},
          {"max_gamma", max_gamma},
          {"max_theta", max_theta},
          {"attribution", by_source.json()}}},
        {"round_trips", std::move(trips)}};
    if (trial.fills) {
        out["fills"] = std::move(fills);
    }
    if (trial.log) {
        out["log"] = std::move(log);
    }
    if (trial.minute_metrics && !pnl.empty()) {
        // P&L and Greeks at the same points, LTTB-downsampled on the P&L so the page only draws.
        constexpr size_t kMaxChartPoints = 1000;
        nlohmann::json chart = columns({"pnl", "x_greek", "delta", "theta", "gamma"});
        chart["day_boundaries"] = day_starts;
        for (const size_t idx : lttb_downsample_indices(pnl, kMaxChartPoints)) {
            chart["pnl"].push_back(pnl[idx]);
            chart["x_greek"].push_back(static_cast<int>(idx));
            chart["delta"].push_back(metrics[idx].delta);
            chart["theta"].push_back(metrics[idx].theta);
            chart["gamma"].push_back(metrics[idx].gamma);
        }
        out["chart"] = std::move(chart);
    }
    return out;
}

/** The output's last line: that the run went through, and the days its trials ran a column each
 * (a day is the same for all of them: any one's). */
auto days_line(std::span<const backtest::BacktestRunSummary> summaries) -> nlohmann::json {
    std::map<std::size_t, const backtest::DailyResult*> by_file;
    for (const backtest::BacktestRunSummary& s : summaries) {
        for (const backtest::DailyResult& d : s.daily_results) {
            if (!d.load_failed && !d.file_path.empty()) {
                by_file.try_emplace(d.file_index, &d);
            }
        }
    }
    nlohmann::json days = columns(
        {"date", "file", "bars", "quotes", "underlying_open", "underlying_close"});
    for (const auto& [index, d] : by_file) {
        days["date"].push_back(date_number(d->file_path));
        days["file"].push_back(d->file_path);
        days["bars"].push_back(d->processed_timesteps);
        days["quotes"].push_back(d->total_rows);
        days["underlying_open"].push_back(d->underlying_open);
        days["underlying_close"].push_back(d->underlying_close);
    }
    return {{"status", "ok"}, {"days", std::move(days)}};
}

/** The output: a line a trial (trial_line), each let go of once written, then days_line. */
void print_results(std::vector<backtest::BacktestRunSummary>& summaries,
                   std::span<const backtest::BacktestTrial> trials,
                   std::span<const std::string> ids) {
    const nlohmann::json last = days_line(summaries);
    for (std::size_t i = 0; i < trials.size(); ++i) {
        print_json(trial_line(summaries[i], trials[i], ids[i]));
        summaries[i] = {};
    }
    print_json(last);
}

/** --batch <spec.json>: many trials over the same days, each day loaded once and its chains priced
 * as the trials ask for them (backtest::run_backtests). The spec: {"files": [...],
 * "risk_free_rate", "iv_price_mode", "cache_dir", "threads", "days_ahead", "daily_dir" (the values
 * by day the strategies read, model::DailyTable), "trials": [{"id",
 * "strategy", "setting": {name: value}, "costs" (read_costs), "fills", "minute_metrics", "log",
 * "log_days" (YYYY-MM-DD: only these days' log), "restart_minute"}]}. Prints print_results, the trials in the
 * spec's order. */
auto run_batch(const std::string& spec_path) -> int {
    std::ifstream in(spec_path);
    if (!in) {
        print_error_json("Cannot read the batch spec " + spec_path);
        return 1;
    }
    const nlohmann::json spec = nlohmann::json::parse(in);
    const std::vector<std::string> files = spec.at("files").get<std::vector<std::string>>();
    const double risk_free_rate = spec.value("risk_free_rate", 0.05);
    const std::string iv_price_mode = spec.value("iv_price_mode", std::string("mid"));
    const model::DailyTable daily = model::DailyTable::load(spec.value("daily_dir", std::string{}));
    std::vector<backtest::BacktestTrial> trials;
    std::vector<std::string> ids;
    for (const nlohmann::json& t : spec.at("trials")) {
        backtest::BacktestTrial trial;
        trial.strategy_name = t.at("strategy").get<std::string>();
        trial.strategy_setting =
            t.value("setting", std::unordered_map<std::string, double>{});
        read_costs(t.value("costs", nlohmann::json::object()), trial);
        trial.fills = t.value("fills", false);
        trial.minute_metrics = t.value("minute_metrics", false);
        trial.log = t.value("log", false);
        trial.restart_minute = t.value("restart_minute", 0);
        trial.daily = &daily;
        for (const std::string& day : t.value("log_days", std::vector<std::string>{})) {
            if (const auto parsed = model::parse_date(day)) {
                trial.log_days.push_back(*parsed);
            } else {
                throw std::invalid_argument("log_days: not a date: " + day);
            }
        }
        trials.push_back(std::move(trial));
        ids.push_back(t.value("id", std::to_string(ids.size())));
    }
    std::vector<backtest::BacktestRunSummary> summaries = backtest::run_backtests(
        files, trials, risk_free_rate, iv_price_mode, utilities::DISABLED,
        spec.value("cache_dir", std::string{}), spec.value("threads", std::size_t{0}),
        spec.value("days_ahead", std::size_t{3}));
    print_results(summaries, trials, ids);
    return 0;
}

} // namespace

int main(int argc, char* argv[]) {
    // A day's arrays (a few hundred KB each, half a GB a day) are mapped on their own, so a day let
    // go goes back to the system; glibc would otherwise raise the threshold after the first frees
    // and keep the days in its heaps, which a batch's threads multiply.
    mallopt(M_MMAP_THRESHOLD, 128 * 1024);
    // A heap grows by 64 MB at a time, not a page: a day loaded is tens of MB of arrays, and the
    // threads' heaps would otherwise grow a page per call, again each day after the trim.
    mallopt(M_TOP_PAD, 64 * 1024 * 1024);
    // The system's and research strategies, plus each --strategies DIR (removed from the args).
    strategy::StrategyRegistry::load(strategy::StrategyRegistry::beside_executable());
    strategy::StrategyRegistry::load(strategy::StrategyRegistry::beside_executable() / "research");
    std::vector<char*> args{argv[0]};
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--strategies" && i + 1 < argc) {
            strategy::StrategyRegistry::load(argv[++i]);
        } else {
            args.push_back(argv[i]);
        }
    }
    argc = static_cast<int>(args.size());
    argv = args.data();
    if (const int fd = ::dup(STDOUT_FILENO); fd >= 0 && ::dup2(STDERR_FILENO, STDOUT_FILENO) >= 0) {
        result_fd = fd;
    }
    if (argc == 3 && std::string(argv[1]) == "--batch") {
        try {
            return run_batch(argv[2]);
        } catch (const std::exception& e) {
            print_error_json(e.what());
            return 1;
        }
    }
    if (argc == 2 && std::string(argv[1]) == "--list-strategies") {
        try {
            print_strategies_json();
            return 0;
        } catch (const std::exception& e) {
            print_error_json(e.what());
            return 1;
        }
    }
    if (argc < 3) {
        print_error_json(
            "Usage: backtest_entry <parquet_path>|<--files file1 file2 ...> <strategy_name> "
            "[--costs JSON (as a batch trial's)] [--risk-free-rate number] "
            "[--iv-price-mode mid|bid|ask] [--daily DIR] [--log] [key=value ...] | "
            "backtest_entry --batch <spec.json> | backtest_entry --list-strategies; any of them "
            "[--strategies DIR]...");
        return 1;
    }

    std::vector<std::string> parquet_files;
    std::string strategy_name;
    nlohmann::json costs = nlohmann::json::object();
    double risk_free_rate = 0.05;
    std::string iv_price_mode = "mid";
    std::string daily_dir;
    utilities::LogLevel log_level = utilities::DISABLED;
    {
        const char* log_env = std::getenv("BACKTEST_LOG");
        if (log_env && (std::string(log_env) == "1" || std::string(log_env) == "true"))
            log_level = utilities::INFO;
    }
    std::unordered_map<std::string, double> strategy_setting;

    int arg_idx = 1;
    if (std::string(argv[1]) == "--files") {
        arg_idx = 2;
        while (arg_idx < argc) {
            const std::string arg = argv[arg_idx];
            if (arg.starts_with("--") || arg.find('=') != std::string::npos) {
                break;
            }
            // The strategy name ends the file list: no slash or dot.
            if (arg.find('/') == std::string::npos && arg.find('.') == std::string::npos &&
                arg.find('\\') == std::string::npos) {
                strategy_name = arg;
                arg_idx++;
                break;
            }
            parquet_files.push_back(arg);
            arg_idx++;
        }
    } else {
        parquet_files.push_back(argv[1]);
        strategy_name = argv[2];
        arg_idx = 3;
    }

    if (parquet_files.empty()) {
        print_error_json("No parquet files specified");
        return 1;
    }
    if (strategy_name.empty()) {
        print_error_json("Strategy name not specified");
        return 1;
    }

    for (int i = arg_idx; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--costs" && i + 1 < argc) {
            try {
                costs = nlohmann::json::parse(argv[++i]);
            } catch (const std::exception& e) {
                print_error_json(std::string("--costs takes a JSON object: ") + e.what());
                return 1;
            }
            continue;
        }
        if (arg == "--risk-free-rate" && i + 1 < argc) {
            try {
                risk_free_rate = std::stod(argv[++i]);
            } catch (...) {
                print_error_json("--risk-free-rate takes a number");
                return 1;
            }
            continue;
        }
        if (arg == "--iv-price-mode" && i + 1 < argc) {
            iv_price_mode = argv[++i];
            continue;
        }
        if (arg == "--daily" && i + 1 < argc) {
            daily_dir = argv[++i];
            continue;
        }
        if (arg == "--log") {
            log_level = utilities::INFO;
            continue;
        }

        // Strategy setting key=value; anything else is a mistake, not something to skip.
        const auto pos = arg.find('=');
        if (pos == std::string::npos || pos == 0 || pos == arg.size() - 1) {
            print_error_json("Unknown argument: " + arg);
            return 1;
        }
        const std::string key = arg.substr(0, pos);
        const std::string val = arg.substr(pos + 1);
        try {
            size_t used = 0;
            const double value = std::stod(val, &used);
            if (used != val.size()) {
                throw std::invalid_argument(val);
            }
            strategy_setting[key] = value;
        } catch (...) {
            print_error_json("Setting " + arg + " is not a number");
            return 1;
        }
    }

    try {
        const model::DailyTable daily = model::DailyTable::load(daily_dir);
        backtest::BacktestTrial trial{.strategy_name = strategy_name,
                                            .strategy_setting = strategy_setting,
                                            .fills = true,
                                            .daily = &daily};
        read_costs(costs, trial);
        std::vector<backtest::BacktestRunSummary> summaries = backtest::run_backtests(
            parquet_files, std::span(&trial, 1), risk_free_rate, iv_price_mode, log_level);
        const std::string id = "0";
        print_results(summaries, std::span(&trial, 1), std::span(&id, 1));
        return 0;
    } catch (const std::exception& e) {
        print_error_json(e.what());
        return 1;
    } catch (...) {
        print_error_json("Unknown C++ backtest error");
        return 1;
    }
}
