#include "ib_settings.hpp"
#include "../../utilities/base_engine.hpp"
#include "../../utilities/log_level.hpp"
#include <charconv>
#include <cstdlib>
#include <format>
#include <fstream>
#include <string_view>
#include <sys/stat.h>

namespace adapters {

auto IbSettings::from_env(const utilities::BaseEngine& log) -> IbSettings {
    const auto env = [](const char* name) -> std::string_view {
        const char* v = std::getenv(name);
        return v != nullptr ? v : "";
    };
    const auto number = [&](const char* name, int& out) {
        const std::string_view v = env(name);
        int n = 0;
        if (v.empty()) {
            return;
        }
        if (const auto [end, ec] = std::from_chars(v.data(), v.data() + v.size(), n);
            ec != std::errc{} || end != v.data() + v.size()) {
            log.write_log(std::format("{}={} is not a number; using {}", name, v, out), utilities::WARNING);
            return;
        }
        out = n;
    };
    IbSettings s;
    if (const std::string_view host = env("IB_HOST"); !host.empty()) {
        s.host = host;
    }
    number("IB_PORT", s.port);
    number("IB_CLIENT_ID", s.client_id);
    if (const std::string_view account = env("IB_ACCOUNT"); !account.empty()) {
        s.account = account;
    }
    number("IB_MARKET_DATA_LINES", s.market_data_lines);
    number("IB_MARKET_DATA_TYPE", s.market_data_type);
    return s;
}

auto IbSettings::is_account_allowed() const -> bool {
    if (account.starts_with('D')) {
        return true; // paper
    }
    struct stat st{};
    if (account.empty() || ::stat(live_account_file.c_str(), &st) != 0 || !S_ISREG(st.st_mode) ||
        st.st_uid != 0 || (st.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
        return false;
    }
    std::ifstream in(live_account_file);
    std::string named;
    in >> named;
    return named == account;
}

} // namespace adapters
