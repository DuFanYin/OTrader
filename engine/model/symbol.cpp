/** The platform's instrument names: parsing and formatting option and chain symbols. */

#include "model/symbol.hpp"
#include "model/utility.hpp"
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <format>

namespace model {

namespace {

constexpr auto right_letter(OptionType type) -> char {
    return type == OptionType::CALL ? 'C' : 'P';
}

} // namespace

auto strike_text(double strike) -> std::string { return std::format("{}", strike); }

auto option_symbol(const OptionId& id) -> std::string {
    return std::format("{}-{:%Y%m%d}-{}-{}", id.root, id.expiry, right_letter(id.type),
                       strike_text(id.strike));
}

auto occ_code(const OptionId& id) -> std::string {
    return std::format("{:%y%m%d}{}{:08}", std::chrono::sys_days{id.expiry}, right_letter(id.type),
                       static_cast<long long>(std::llround(id.strike * 1000.0)));
}

auto parse_occ_code(std::string_view root, std::string_view code) -> std::optional<OptionId> {
    if (code.size() < 15) {
        return std::nullopt;
    }
    const auto number = [&](std::size_t at, std::size_t width) -> std::optional<int> {
        int value = 0;
        const char* first = code.data() + at;
        const auto [end, ec] = std::from_chars(first, first + width, value);
        return ec == std::errc{} && end == first + width ? std::optional(value) : std::nullopt;
    };
    const auto yy = number(0, 2);
    const auto mm = number(2, 2);
    const auto dd = number(4, 2);
    const auto strike = number(7, 8);
    const char right = static_cast<char>(std::toupper(static_cast<unsigned char>(code[6])));
    if (!yy || !mm || !dd || !strike || (right != 'C' && right != 'P')) {
        return std::nullopt;
    }
    const std::chrono::year_month_day expiry{
        std::chrono::year{*yy < 80 ? 2000 + *yy : 1900 + *yy},
        std::chrono::month{static_cast<unsigned>(*mm)}, std::chrono::day{static_cast<unsigned>(*dd)}};
    if (!expiry.ok()) {
        return std::nullopt;
    }
    return OptionId{.root = std::string(root),
                    .expiry = expiry,
                    .type = right == 'C' ? OptionType::CALL : OptionType::PUT,
                    .strike = *strike / 1000.0};
}

auto chain_symbol(std::string_view root, std::chrono::year_month_day expiry) -> std::string {
    return std::format("{}_{:%Y%m%d}", root, expiry);
}

auto parse_option_symbol(std::string_view symbol) -> std::optional<OptionId> {
    // Exactly four fields: three '-'.
    std::array<std::string_view, 4> fields;
    std::size_t start = 0;
    for (std::size_t i = 0; i < 3; ++i) {
        const std::size_t dash = symbol.find('-', start);
        if (dash == std::string_view::npos) {
            return std::nullopt;
        }
        fields[i] = symbol.substr(start, dash - start);
        start = dash + 1;
    }
    fields[3] = symbol.substr(start);
    if (fields[3].contains('-') || fields[0].empty() || fields[2].size() != 1) {
        return std::nullopt;
    }
    OptionId id;
    id.root = fields[0];
    const auto expiry = fields[1].size() == 8 ? parse_date(fields[1]) : std::nullopt;
    if (!expiry) {
        return std::nullopt;
    }
    id.expiry = *expiry;
    switch (fields[2][0]) {
    case 'C':
        id.type = OptionType::CALL;
        break;
    case 'P':
        id.type = OptionType::PUT;
        break;
    default:
        return std::nullopt;
    }
    const std::string_view strike = fields[3];
    const auto [end, ec] = std::from_chars(strike.data(), strike.data() + strike.size(), id.strike);
    if (ec != std::errc{} || end != strike.data() + strike.size() || id.strike <= 0.0) {
        return std::nullopt;
    }
    return id;
}

} // namespace model
