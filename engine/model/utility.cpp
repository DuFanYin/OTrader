#include "model/utility.hpp"
#include <algorithm>
#include <array>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace model {

namespace {

auto new_york() -> const std::chrono::time_zone* {
    static const std::chrono::time_zone* const zone = std::chrono::locate_zone("America/New_York");
    return zone;
}

/** A date as written: on(2025, 7, 3). */
constexpr auto on(int y, unsigned m, unsigned d) -> std::chrono::year_month_day {
    return std::chrono::year_month_day{std::chrono::year{y}, std::chrono::month{m},
                                       std::chrono::day{d}};
}

// The New York Stock Exchange's holidays and early closes (13:00), as it publishes them.
constexpr std::chrono::year kFirstListedYear{2020};
constexpr std::chrono::year kLastListedYear{2027};
constexpr std::array kHolidays{
    on(2020, 1, 1), on(2020, 1, 20), on(2020, 2, 17), on(2020, 4, 10), on(2020, 5, 25),
    on(2020, 7, 3), on(2020, 9, 7), on(2020, 11, 26), on(2020, 12, 25),
    on(2021, 1, 1), on(2021, 1, 18), on(2021, 2, 15), on(2021, 4, 2), on(2021, 5, 31),
    on(2021, 7, 5), on(2021, 9, 6), on(2021, 11, 25), on(2021, 12, 24),
    on(2022, 1, 17), on(2022, 2, 21), on(2022, 4, 15), on(2022, 5, 30), on(2022, 6, 20),
    on(2022, 7, 4), on(2022, 9, 5), on(2022, 11, 24), on(2022, 12, 26),
    on(2023, 1, 2), on(2023, 1, 16), on(2023, 2, 20), on(2023, 4, 7), on(2023, 5, 29),
    on(2023, 6, 19), on(2023, 7, 4), on(2023, 9, 4), on(2023, 11, 23), on(2023, 12, 25),
    on(2024, 1, 1), on(2024, 1, 15), on(2024, 2, 19), on(2024, 3, 29), on(2024, 5, 27),
    on(2024, 6, 19), on(2024, 7, 4), on(2024, 9, 2), on(2024, 11, 28), on(2024, 12, 25),
    // 2025-01-09: closed for the national day of mourning for President Carter
    on(2025, 1, 1), on(2025, 1, 9), on(2025, 1, 20), on(2025, 2, 17), on(2025, 4, 18),
    on(2025, 5, 26), on(2025, 6, 19), on(2025, 7, 4), on(2025, 9, 1), on(2025, 11, 27),
    on(2025, 12, 25),
    on(2026, 1, 1), on(2026, 1, 19), on(2026, 2, 16), on(2026, 4, 3), on(2026, 5, 25),
    on(2026, 6, 19), on(2026, 7, 3), on(2026, 9, 7), on(2026, 11, 26), on(2026, 12, 25),
    on(2027, 1, 1), on(2027, 1, 18), on(2027, 2, 15), on(2027, 3, 26), on(2027, 5, 31),
    on(2027, 6, 18), on(2027, 7, 5), on(2027, 9, 6), on(2027, 11, 25), on(2027, 12, 24),
};
constexpr std::array kEarlyCloses{
    on(2020, 11, 27), on(2020, 12, 24), on(2021, 11, 26), on(2022, 11, 25),
    on(2023, 7, 3), on(2023, 11, 24),
    on(2024, 7, 3), on(2024, 11, 29), on(2024, 12, 24),
    on(2025, 7, 3), on(2025, 11, 28), on(2025, 12, 24),
    on(2026, 11, 27), on(2026, 12, 24),
    on(2027, 11, 26),
};

} // namespace

auto is_trading_day(std::chrono::year_month_day date) -> bool {
    using namespace std::chrono;
    const weekday wd{sys_days{date}};
    if (wd == Saturday || wd == Sunday) {
        return false;
    }
    return date.year() < kFirstListedYear || date.year() > kLastListedYear ||
           std::ranges::find(kHolidays, date) == kHolidays.end();
}

auto next_trading_day(std::chrono::year_month_day date) -> std::chrono::year_month_day {
    using namespace std::chrono;
    year_month_day next = date;
    do {
        next = year_month_day{sys_days{next} + days{1}};
    } while (!is_trading_day(next));
    return next;
}

auto session_close(std::chrono::year_month_day date) -> std::chrono::minutes {
    using namespace std::chrono;
    return std::ranges::find(kEarlyCloses, date) != kEarlyCloses.end() ? minutes{13h}
                                                                        : minutes{16h};
}

auto round_to(double value, double target) -> double {
    if (target <= 0) {
        return value;
    }
    return std::round(value / target) * target;
}

auto years_to_expiry(std::chrono::system_clock::time_point now,
                     std::chrono::system_clock::time_point expiry) -> double {
    const auto secs = std::chrono::duration_cast<std::chrono::seconds>(expiry - now).count();
    if (secs <= 0) {
        return 0.0;
    }
    return std::max(1e-6, static_cast<double>(secs) / (365.25 * 24.0 * 3600.0));
}

auto new_york_time(std::chrono::year_month_day date, std::chrono::minutes time_of_day)
    -> std::chrono::system_clock::time_point {
    using namespace std::chrono;
    return zoned_time{new_york(), local_days{date} + time_of_day}.get_sys_time();
}

auto option_expiry_time(std::chrono::year_month_day date) -> std::chrono::system_clock::time_point {
    // Asked of every option held at every bar; the zone's lookup takes a lock, so the last is kept.
    thread_local std::pair<std::chrono::year_month_day, std::chrono::system_clock::time_point> last{};
    if (last.first != date) {
        last = {date, new_york_time(date, session_close(date))};
    }
    return last.second;
}

namespace {

/** t on New York's clock. The zone's lookup takes a lock, so each thread keeps the last offset it
 * found and the span it holds for (until the next daylight saving change). */
auto new_york_local(std::chrono::system_clock::time_point t)
    -> std::chrono::local_time<std::chrono::system_clock::duration> {
    using namespace std::chrono;
    thread_local sys_info span{}; // empty: looked up first
    if (t < span.begin || t >= span.end) {
        span = new_york()->get_info(t);
    }
    return local_time<system_clock::duration>{(t + span.offset).time_since_epoch()};
}

} // namespace

auto new_york_date(std::chrono::system_clock::time_point t) -> std::chrono::year_month_day {
    using namespace std::chrono;
    return year_month_day{floor<days>(new_york_local(t))};
}

auto minutes_since_open(std::chrono::system_clock::time_point t) -> int {
    using namespace std::chrono;
    const auto local = new_york_local(t);
    const auto open = floor<days>(local) + 9h + 30min;
    return static_cast<int>(floor<minutes>(local - open).count());
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
