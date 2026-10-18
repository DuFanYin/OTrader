/**
 * Compares the benchmarks' rows with baseline.tsv, so that a change that slows a hot path is caught.
 *
 * The benchmarks write their rows (section, row, p50, p99 in ns) to the file OPTRA_BENCH_ROWS names
 * (bench_util.hpp's print_row); scripts/bench.sh --check runs them three times, each into its own
 * file. A row in several files counts as the median of each. The baseline's rows are the ones held
 * to: a p50 more than 15% above its baseline, or a p99 more than 50%, is a regression (exit 1). With
 * --write, those medians become the baseline (after an intended change, with the clock fixed).
 *
 *   bench_check ROWS.tsv...            compare
 *   bench_check --write ROWS.tsv...    make them the baseline
 */
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr double kP50Rise = 0.15;
constexpr double kP99Rise = 0.50;

using Key = std::pair<std::string, std::string>;  // section, row

struct Baseline {
    std::vector<Key> rows;  // in the file's order
    std::map<Key, std::pair<long long, long long>> ns;
};

std::vector<std::string> split_tabs(const std::string& line) {
    std::vector<std::string> fields;
    std::stringstream in(line);
    for (std::string field; std::getline(in, field, '\t');) fields.push_back(field);
    return fields;
}

Baseline read_baseline() {
    Baseline base;
    std::ifstream in(OPTRA_BENCH_BASELINE);
    for (std::string line; std::getline(in, line);) {
        if (line.empty() || line[0] == '#') continue;
        auto f = split_tabs(line);
        if (f.size() != 4) continue;
        Key key{f[0], f[1]};
        base.rows.push_back(key);
        base.ns[key] = {std::stoll(f[2]), std::stoll(f[3])};
    }
    return base;
}

long long median(std::vector<long long> v) {
    std::sort(v.begin(), v.end());
    auto n = v.size();
    return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
}

// Each row's median p50 and p99 over the files given.
std::map<Key, std::pair<long long, long long>> read_runs(const std::vector<std::string>& files) {
    std::map<Key, std::pair<std::vector<long long>, std::vector<long long>>> seen;
    for (const auto& file : files) {
        std::ifstream in(file);
        for (std::string line; std::getline(in, line);) {
            auto f = split_tabs(line);
            if (f.size() != 4) continue;
            auto& [p50, p99] = seen[{f[0], f[1]}];
            p50.push_back(std::stoll(f[2]));
            p99.push_back(std::stoll(f[3]));
        }
    }
    std::map<Key, std::pair<long long, long long>> runs;
    for (auto& [key, values] : seen) runs[key] = {median(values.first), median(values.second)};
    return runs;
}

int write_baseline(const Baseline& base, const std::map<Key, std::pair<long long, long long>>& runs) {
    for (const auto& key : base.rows) {
        if (!runs.contains(key)) {
            std::fprintf(stderr, "not in the runs: %s / %s\n", key.first.c_str(), key.second.c_str());
            return 2;
        }
    }
    std::ofstream out(OPTRA_BENCH_BASELINE);
    out << "# section\trow\tp50 ns\tp99 ns: the median of three runs on the ThinkPad at 1.9 GHz, boost "
           "off (scripts/bench.sh clock on); scripts/bench.sh baseline\n";
    for (const auto& key : base.rows) {
        const auto& [p50, p99] = runs.at(key);
        out << key.first << '\t' << key.second << '\t' << p50 << '\t' << p99 << '\n';
    }
    std::printf("baseline written: %s (%zu rows)\n", OPTRA_BENCH_BASELINE, base.rows.size());
    return 0;
}

int compare(const Baseline& base, const std::map<Key, std::pair<long long, long long>>& runs) {
    int worse = 0;
    std::printf("%-60s %9s %9s %9s %9s\n", "row", "p50 base", "now", "p99 base", "now");
    for (const auto& key : base.rows) {
        auto label = (key.first.substr(0, 24) + "... " + key.second).substr(0, 60);
        auto found = runs.find(key);
        if (found == runs.end()) {
            std::printf("%-60s not measured\n", label.c_str());
            ++worse;
            continue;
        }
        auto [b50, b99] = base.ns.at(key);
        auto [n50, n99] = found->second;
        bool bad = n50 > b50 * (1 + kP50Rise) || n99 > b99 * (1 + kP99Rise);
        worse += bad;
        std::printf("%-60s %8.1fu %8.1fu %8.1fu %8.1fu%s\n", label.c_str(), b50 / 1000.0, n50 / 1000.0,
                    b99 / 1000.0, n99 / 1000.0, bad ? "  WORSE" : n50 < b50 * 0.8 ? "  faster" : "");
    }
    if (worse) {
        std::printf("\n%d row(s) slower than the baseline allows (p50 +%.0f%%, p99 +%.0f%%)\n", worse,
                    kP50Rise * 100, kP99Rise * 100);
        return 1;
    }
    std::printf("\nno regression\n");
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    bool write = argc > 1 && std::string(argv[1]) == "--write";
    std::vector<std::string> files(argv + 1 + write, argv + argc);
    if (files.empty()) {
        std::fprintf(stderr, "usage: bench_check [--write] ROWS.tsv...\n");
        return 2;
    }
    auto base = read_baseline();
    auto runs = read_runs(files);
    return write ? write_baseline(base, runs) : compare(base, runs);
}
