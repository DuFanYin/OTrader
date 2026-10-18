#pragma once

/** Values by date, one series a name: data prepared outside the engine (data/daily/<name>.csv, a
 * line "YYYY-MM-DD,value" each; a header and '#' lines skipped), the same file for a backtest and
 * live. A strategy reads them as of the day before (strategy::Strategy::daily). */

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace model {

class DailyTable {
  public:
    using Day = std::chrono::year_month_day;

    /** Every <name>.csv in dir, the series named after the file; none when dir does not exist.
     * Throws on a line that is not a date and a number, or a date given twice. */
    static DailyTable load(const std::filesystem::path& dir);

    /** The series' values, in any order: kept by date. Throws on a date given twice. */
    void set(std::string series, std::vector<std::pair<Day, double>> values);

    /** The series' last `n` values dated before `day` (fewer when it has fewer), oldest first. */
    [[nodiscard]] std::vector<double> before(std::string_view series, Day day,
                                             std::size_t n) const;
    /** The series' first date on or after `day`. */
    [[nodiscard]] std::optional<Day> next(std::string_view series, Day day) const;
    [[nodiscard]] bool empty() const { return series_.empty(); }

  private:
    std::map<std::string, std::vector<std::pair<Day, double>>, std::less<>> series_;
};

} // namespace model
