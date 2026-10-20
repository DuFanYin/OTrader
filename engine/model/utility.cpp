#include "model/utility.hpp"
#include <algorithm>
#include <array>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace model {

auto years_to_expiry(std::chrono::system_clock::time_point now,
                     std::chrono::system_clock::time_point expiry) -> double {
    const auto secs = std::chrono::duration_cast<std::chrono::seconds>(expiry - now).count();
    if (secs <= 0) {
        return 0.0;
    }
    return std::max(1e-6, static_cast<double>(secs) / kSecondsAYear);
}

auto datetime_to_str(std::chrono::system_clock::time_point tp) -> std::string {
    return std::format("{:%Y-%m-%d %H:%M:%S}", std::chrono::floor<std::chrono::seconds>(tp));
}

auto parse_date(std::string_view text) -> std::optional<std::chrono::year_month_day> {
    const bool dashed = text.size() >= 10 && text[4] == '-' && text[7] == '-';
    if (!dashed && text.size() < 8) {
        return std::nullopt;
    }
    // The number the digits at `at` make; -1 when one of them is not a digit.
    const auto digits = [&](std::size_t at, std::size_t width) -> int {
        int value = 0;
        for (const char c : text.substr(at, width)) {
            if (c < '0' || c > '9') {
                return -1;
            }
            value = value * 10 + (c - '0');
        }
        return value;
    };
    const int y = digits(0, 4);
    const int m = digits(dashed ? 5 : 4, 2);
    const int d = digits(dashed ? 8 : 6, 2);
    if (y < 0 || m < 0 || d < 0) {
        return std::nullopt;
    }
    const std::chrono::year_month_day date{std::chrono::year{y},
                                           std::chrono::month{static_cast<unsigned>(m)},
                                           std::chrono::day{static_cast<unsigned>(d)}};
    return date.ok() ? std::optional(date) : std::nullopt;
}

auto BarGate::next(std::chrono::system_clock::time_point event_time) -> bool {
    const auto bar = std::chrono::floor<std::chrono::minutes>(event_time);
    if (bar <= last_bar_) {
        return false;
    }
    last_bar_ = bar;
    if (++bars_ < every_) {
        return false;
    }
    bars_ = 0;
    return true;
}

} // namespace model
