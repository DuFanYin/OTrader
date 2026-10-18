#include "model/daily_table.hpp"
#include "model/utility.hpp"

#include <algorithm>
#include <charconv>
#include <format>
#include <fstream>
#include <stdexcept>

namespace model {

auto DailyTable::load(const std::filesystem::path& dir) -> DailyTable {
    DailyTable table;
    std::error_code error;
    if (!std::filesystem::is_directory(dir, error)) {
        return table;
    }
    for (const auto& file : std::filesystem::directory_iterator(dir)) {
        if (file.path().extension() != ".csv") {
            continue;
        }
        std::ifstream in(file.path());
        std::vector<std::pair<Day, double>> values;
        std::string line;
        for (int n = 1; std::getline(in, line); ++n) {
            if (line.empty() || line.starts_with('#') || line.starts_with("date")) {
                continue;
            }
            const std::size_t comma = line.find(',');
            const std::optional<Day> day = parse_date(line.substr(0, comma));
            double value = 0.0;
            const char* begin = line.data() + (comma == std::string::npos ? line.size() : comma + 1);
            const auto [end, ec] = std::from_chars(begin, line.data() + line.size(), value);
            if (!day || comma == std::string::npos || ec != std::errc{}) {
                throw std::runtime_error(
                    std::format("{} line {}: not a date and a number", file.path().string(), n));
            }
            values.emplace_back(*day, value);
        }
        table.set(file.path().stem().string(), std::move(values));
    }
    return table;
}

void DailyTable::set(std::string series, std::vector<std::pair<Day, double>> values) {
    std::ranges::sort(values, {}, &std::pair<Day, double>::first);
    if (std::ranges::adjacent_find(values, {}, &std::pair<Day, double>::first) != values.end()) {
        throw std::runtime_error(std::format("{}: a date given twice", series));
    }
    series_.insert_or_assign(std::move(series), std::move(values));
}

auto DailyTable::before(std::string_view series, Day day, std::size_t n) const
    -> std::vector<double> {
    const auto found = series_.find(series);
    if (found == series_.end()) {
        return {};
    }
    const auto& values = found->second;
    const auto end = std::ranges::lower_bound(values, day, {}, &std::pair<Day, double>::first);
    const auto begin = end - std::min<std::ptrdiff_t>(static_cast<std::ptrdiff_t>(n),
                                                      end - values.begin());
    std::vector<double> out;
    for (auto it = begin; it != end; ++it) {
        out.push_back(it->second);
    }
    return out;
}

auto DailyTable::next(std::string_view series, Day day) const -> std::optional<Day> {
    const auto found = series_.find(series);
    if (found == series_.end()) {
        return std::nullopt;
    }
    const auto it = std::ranges::lower_bound(found->second, day, {}, &std::pair<Day, double>::first);
    return it != found->second.end() ? std::optional(it->first) : std::nullopt;
}

} // namespace model
