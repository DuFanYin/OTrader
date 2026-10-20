#include "model/session_calendar.hpp"
#include <algorithm>
#include <array>
#include <cstddef>
#include <tuple>
#include <utility>

namespace model {

namespace {

/** A date as written: on(2025, 7, 3). */
constexpr auto on(int y, unsigned m, unsigned d) -> std::chrono::year_month_day {
    return std::chrono::year_month_day{std::chrono::year{y}, std::chrono::month{m},
                                       std::chrono::day{d}};
}

// The New York Stock Exchange's holidays and early closes (13:00), as it publishes them.
constexpr std::array kNyseHolidays{
    on(2016, 1, 1), on(2016, 1, 18), on(2016, 2, 15), on(2016, 3, 25), on(2016, 5, 30),
    on(2016, 7, 4), on(2016, 9, 5), on(2016, 11, 24), on(2016, 12, 26),
    on(2017, 1, 2), on(2017, 1, 16), on(2017, 2, 20), on(2017, 4, 14), on(2017, 5, 29),
    on(2017, 7, 4), on(2017, 9, 4), on(2017, 11, 23), on(2017, 12, 25),
    // 2018-12-05: closed for the national day of mourning for President George H. W. Bush
    on(2018, 1, 1), on(2018, 1, 15), on(2018, 2, 19), on(2018, 3, 30), on(2018, 5, 28),
    on(2018, 7, 4), on(2018, 9, 3), on(2018, 11, 22), on(2018, 12, 5), on(2018, 12, 25),
    on(2019, 1, 1), on(2019, 1, 21), on(2019, 2, 18), on(2019, 4, 19), on(2019, 5, 27),
    on(2019, 7, 4), on(2019, 9, 2), on(2019, 11, 28), on(2019, 12, 25),
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
constexpr std::array kNyseEarlyCloses{
    on(2016, 11, 25), on(2017, 7, 3), on(2017, 11, 24),
    on(2018, 7, 3), on(2018, 11, 23), on(2018, 12, 24),
    on(2019, 7, 3), on(2019, 11, 29), on(2019, 12, 24),
    on(2020, 11, 27), on(2020, 12, 24), on(2021, 11, 26), on(2022, 11, 25),
    on(2023, 7, 3), on(2023, 11, 24),
    on(2024, 7, 3), on(2024, 11, 29), on(2024, 12, 24),
    on(2025, 7, 3), on(2025, 11, 28), on(2025, 12, 24),
    on(2026, 11, 27), on(2026, 12, 24),
    on(2027, 11, 26),
};

/** The calendar's zone. Looking one up takes the time zone database's lock: each thread keeps the
 * last it found. */
auto zone_of(const SessionCalendar& c) -> const std::chrono::time_zone* {
    thread_local std::pair<const SessionCalendar*, const std::chrono::time_zone*> last{};
    if (last.first != &c) {
        last = {&c, std::chrono::locate_zone(c.zone)};
    }
    return last.second;
}

/** t on the calendar's clock. Each thread keeps the offsets it found last (a few calendars) and
 * the span each holds for (until the next daylight saving change). */
auto local_of(const SessionCalendar& c, std::chrono::system_clock::time_point t)
    -> std::chrono::local_time<std::chrono::system_clock::duration> {
    using namespace std::chrono;
    thread_local std::array<std::pair<const SessionCalendar*, sys_info>, 4> spans{};
    thread_local std::size_t next = 0;
    auto found = std::ranges::find(spans, &c, [](const auto& s) { return s.first; });
    if (found == spans.end()) {
        found = spans.begin() + static_cast<std::ptrdiff_t>(next++ % spans.size());
        *found = {&c, sys_info{}};
    }
    sys_info& span = found->second;
    if (t < span.begin || t >= span.end) {
        span = zone_of(c)->get_info(t);
    }
    return local_time<system_clock::duration>{(t + span.offset).time_since_epoch()};
}

/** The share of a session `length` minutes long done `minute` minutes after its open. */
auto session_done(double minute, double length) -> double {
    const auto upto = [](double m) {
        double sum = 0.0;
        for (std::size_t b = 0; b < kSessionVarianceShares.size(); ++b) {
            const double from = 30.0 * static_cast<double>(b);
            sum += kSessionVarianceShares[b] * std::clamp((m - from) / 30.0, 0.0, 1.0);
        }
        return sum;
    };
    const double whole = upto(length);
    return whole > 0.0 ? upto(std::clamp(minute, 0.0, length)) / whole : 1.0;
}

} // namespace

const SessionCalendar kNyse{.name = "NYSE",
                            .zone = "America/New_York",
                            .holidays = kNyseHolidays,
                            .early_closes = kNyseEarlyCloses,
                            .first_listed = std::chrono::year{2016},
                            .last_listed = std::chrono::year{2027},
                            .open = std::chrono::minutes{9 * 60 + 30},
                            .close = std::chrono::minutes{16 * 60},
                            .early_close = std::chrono::minutes{13 * 60},
                            .roll = std::chrono::minutes{8 * 60 + 45}};

auto SessionCalendar::is_trading_day(std::chrono::year_month_day date) const -> bool {
    using namespace std::chrono;
    const weekday wd{sys_days{date}};
    if (weekends_closed && (wd == Saturday || wd == Sunday)) {
        return false;
    }
    return date.year() < first_listed || date.year() > last_listed ||
           std::ranges::find(holidays, date) == holidays.end();
}

auto SessionCalendar::next_trading_day(std::chrono::year_month_day date) const
    -> std::chrono::year_month_day {
    using namespace std::chrono;
    year_month_day next = date;
    do {
        next = year_month_day{sys_days{next} + days{1}};
    } while (!is_trading_day(next));
    return next;
}

auto SessionCalendar::session_close(std::chrono::year_month_day date) const
    -> std::chrono::minutes {
    return std::ranges::find(early_closes, date) != early_closes.end() ? early_close : close;
}

auto SessionCalendar::at(std::chrono::year_month_day date, std::chrono::minutes time_of_day) const
    -> std::chrono::system_clock::time_point {
    using namespace std::chrono;
    return zoned_time{zone_of(*this), local_days{date} + time_of_day}.get_sys_time();
}

auto SessionCalendar::expiry_time(std::chrono::year_month_day date) const
    -> std::chrono::system_clock::time_point {
    // Asked of every option held at every bar: the last answer is kept.
    thread_local std::tuple<const SessionCalendar*, std::chrono::year_month_day,
                            std::chrono::system_clock::time_point>
        last{};
    if (std::get<0>(last) != this || std::get<1>(last) != date) {
        last = {this, date, at(date, session_close(date))};
    }
    return std::get<2>(last);
}

auto SessionCalendar::date_of(std::chrono::system_clock::time_point t) const
    -> std::chrono::year_month_day {
    return std::chrono::year_month_day{std::chrono::floor<std::chrono::days>(local_of(*this, t))};
}

auto SessionCalendar::variance_years(std::chrono::system_clock::time_point t,
                                     std::chrono::system_clock::time_point expiry,
                                     double overnight) const -> double {
    using namespace std::chrono;
    if (expiry <= t) {
        return 0.0;
    }
    const auto minutes_after_open = [this](system_clock::time_point at) {
        const auto local = local_of(*this, at);
        return duration<double, std::ratio<60>>(local - (floor<days>(local) + open)).count();
    };
    const year_month_day first = date_of(t);
    const year_month_day last = date_of(expiry);
    double days_of_variance = 0.0;
    for (sys_days d{first}; d <= sys_days{last}; d += days{1}) {
        const year_month_day date{d};
        if (!is_trading_day(date)) {
            continue;
        }
        const double length = static_cast<double>((session_close(date) - open).count());
        const double from = date == first ? minutes_after_open(t) : -1.0;
        const double to = date == last ? minutes_after_open(expiry) : length;
        if (from < 0.0) {
            days_of_variance += overnight; // the night before this open is still to come
        }
        days_of_variance += (1.0 - overnight) *
                            std::max(0.0, session_done(to, length) - session_done(from, length));
    }
    return days_of_variance / 252.0;
}

auto SessionCalendar::minutes_since_open(std::chrono::system_clock::time_point t) const -> int {
    using namespace std::chrono;
    const auto local = local_of(*this, t);
    return static_cast<int>(floor<minutes>(local - (floor<days>(local) + open)).count());
}

} // namespace model
