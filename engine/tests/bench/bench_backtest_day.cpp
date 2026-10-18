/**
 * Where a backtest day's time and memory go: reading the day's contracts and minutes from the
 * Parquet file (HistoricalData::load), reading and pricing every chain of it (load_all_chains),
 * and running one strategy over the day (backtest::Backtest::use_day and run). Days run in
 * order on one engine, as run_backtest_multi runs them, but each step one at a time, so each is
 * timed on its own: wall time, the process's CPU time (pricing is parallel), and resident memory
 * after it.
 *
 * Also the size of what pricing adds to a day (IV and Greeks for every option at every minute),
 * and with --cache DIR the time to load a day and all its chains through a cache there: cold
 * (read, priced and stored) and warm (its stored files read, the Greeks priced again from the
 * stored IV, in place of the reading and the pricing).
 *
 * Usage: bench_backtest_day <strategy> <day.parquet>... [--set name=value]... [--cache DIR]
 */

#include "tests/bench/bench_util.hpp"

#include "tests/lone_backtest.hpp"

#include <sys/resource.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

double cpu_seconds() {
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    const auto seconds = [](const timeval& t) { return t.tv_sec + t.tv_usec / 1e6; };
    return seconds(usage.ru_utime) + seconds(usage.ru_stime);
}

/** VmRSS or VmHWM from /proc/self/status, in MB. */
double memory_mb(const std::string& field) {
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        if (line.starts_with(field + ":")) {
            return std::stod(line.substr(field.size() + 1)) / 1024.0;
        }
    }
    return 0.0;
}

struct Step {
    double wall = 0.0;
    double cpu = 0.0;
};

template <typename F> Step timed(F&& f) {
    const auto wall0 = std::chrono::steady_clock::now();
    const double cpu0 = cpu_seconds();
    f();
    return {std::chrono::duration<double>(std::chrono::steady_clock::now() - wall0).count(),
            cpu_seconds() - cpu0};
}

} // namespace

int main(int argc, char** argv) {
    bench::print_env();
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <strategy> <day.parquet>... [--set name=value]...\n",
                     argv[0]);
        return 2;
    }
    const std::string strategy = argv[1];
    std::vector<std::string> files;
    std::unordered_map<std::string, double> setting;
    std::string cache_dir;
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--cache" && i + 1 < argc) {
            cache_dir = argv[++i];
        } else if (arg == "--set" && i + 1 < argc) {
            const std::string kv = argv[++i];
            const auto eq = kv.find('=');
            setting[kv.substr(0, eq)] = std::stod(kv.substr(eq + 1));
        } else {
            files.push_back(arg);
        }
    }

    test::LoneBacktest backtest;
    core::Main& engine = backtest.main();
    backtest.configure_execution({.fee_rate = 1.2});
    std::string name;
    Step total_load;
    Step total_price;
    Step total_run;
    Step total_save;
    Step total_read;
    double priced_mb_total = 0.0;
    std::printf("%-10s %6s %6s | %8s %8s | %8s %8s | %8s %8s | %8s %8s | %8s %8s\n", "day",
                "minutes", "options", "load s", "cpu", "price s", "cpu", "run s", "cpu", "cold s",
                "warm s", "RSS MB", "priced MB");
    for (const std::string& path : files) {
        const adapters::LoadedDay day = std::make_shared<adapters::HistoricalData>();
        day->set_risk_free_rate(0.05);
        day->set_iv_price_mode("mid");
        const Step load = timed([&] { day->load(path); });
        if (!day->has_data()) {
            std::fprintf(stderr, "cannot load %s\n", path.c_str());
            return 1;
        }
        const Step price = timed([&] { day->load_all_chains(); }); // read and priced
        const std::size_t minutes = day->minutes();
        const std::size_t options = day->listing()->size();
        // What pricing adds per minute: iv, delta, gamma, theta, vega per option; the forwards and
        // expiry times are a few numbers per chain, left out.
        const double priced_mb = static_cast<double>(minutes * options * 5 * sizeof(double)) / 1e6;
        priced_mb_total += priced_mb;

        Step save;
        Step read;
        if (!cache_dir.empty()) {
            std::filesystem::remove(adapters::stored_day(cache_dir, path));
            for (const std::string& chain : day->loaded_chains()) {
                std::filesystem::remove(adapters::stored_chain(
                    cache_dir, path, chain, adapters::pricing_fingerprint(), 0.05, "mid"));
            }
            adapters::HistoricalData cold;
            save = timed([&] {
                cold.load(path, cache_dir);
                cold.load_all_chains();
            });
            adapters::HistoricalData warm;
            read = timed([&] {
                warm.load(path, cache_dir);
                warm.load_all_chains();
            });
            if (!adapters::day_stored(cache_dir, path)) {
                std::fprintf(stderr, "not stored: %s\n", path.c_str());
            }
        }
        total_save.wall += save.wall;
        total_read.wall += read.wall;

        const std::string day_name = path.substr(path.find_last_of('/') + 1, 8);
        const Step run = timed([&] {
            backtest.use_day(day);
            if (name.empty()) {
                name = engine.add_strategy(strategy, backtest.historical_data()->portfolio_name(),
                                           setting);
            }
            (void)backtest.run();
        });
        total_load.wall += load.wall;
        total_load.cpu += load.cpu;
        total_price.wall += price.wall;
        total_price.cpu += price.cpu;
        total_run.wall += run.wall;
        total_run.cpu += run.cpu;
        std::printf("%-10s %6zu %6zu | %8.2f %8.2f | %8.2f %8.2f | %8.3f %8.3f | %8.2f %8.2f | "
                    "%8.0f %8.1f\n",
                    day_name.c_str(), minutes, options, load.wall, load.cpu, price.wall, price.cpu,
                    run.wall, run.cpu, save.wall, read.wall, memory_mb("VmRSS"), priced_mb);
        std::fflush(stdout);
    }
    const double n = static_cast<double>(files.size());
    std::printf("\nper day     load %.2f s (cpu %.2f)   price %.2f s (cpu %.2f)   run %.3f s "
                "(cpu %.3f)\n",
                total_load.wall / n, total_load.cpu / n, total_price.wall / n,
                total_price.cpu / n, total_run.wall / n, total_run.cpu / n);
    if (!cache_dir.empty()) {
        std::printf("cache       cold %.2f s, warm %.3f s a day\n", total_save.wall / n,
                    total_read.wall / n);
    }
    std::printf("peak RSS    %.0f MB;  priced values %.1f MB a day (raw doubles)\n",
                memory_mb("VmHWM"), priced_mb_total / n);
    return 0;
}
