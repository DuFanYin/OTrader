#include "adapters/ib_api/ib_api.hpp"
#include "model/constant.hpp"
#include "model/portfolio_spec.hpp"
#include "model/symbol.hpp"
#include "model/utility.hpp"
#include "utilities/log_level.hpp"
#include "utilities/log_source.hpp"
#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <format>
#include <fstream>
#include <memory>
#include <optional>
#include <string_view>
#include <sys/stat.h>

namespace adapters {

// ---- Settings
auto IbSettings::from_env(const utilities::LogSource& log) -> IbSettings {
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

namespace {

/** A regular file root owns that no one else may write. */
auto root_only(const std::string& path) -> bool {
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == 0 &&
           (st.st_mode & (S_IWGRP | S_IWOTH)) == 0;
}

} // namespace

auto IbSettings::is_account_allowed() const -> bool {
    if (account.starts_with('D')) {
        return true; // paper
    }
    if (account.empty() || !root_only(live_account_file)) {
        return false;
    }
    std::ifstream in(live_account_file);
    std::string named;
    in >> named;
    return named == account;
}

auto IbSettings::live_strategies() const -> std::vector<std::string> {
    std::vector<std::string> lines;
    if (!root_only(live_strategies_file)) {
        return lines;
    }
    std::ifstream in(live_strategies_file);
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && !line.starts_with('#')) {
            lines.push_back(line);
        }
    }
    return lines;
}

// ---- Contracts
namespace {

auto product_of(const std::string& sec_type) -> model::Product {
    if (sec_type == "OPT") {
        return model::Product::OPTION;
    }
    if (sec_type == "IND") {
        return model::Product::INDEX;
    }
    return sec_type == "STK" ? model::Product::EQUITY : model::Product::UNKNOWN;
}

auto option_type_of(const std::string& right) -> model::OptionType {
    return right.starts_with('P') ? model::OptionType::PUT : model::OptionType::CALL;
}

} // namespace

auto platform_symbol(const Contract& c) -> std::string {
    if (c.secType == "OPT" || c.secType == "FOP") {
        if (const auto expiry = model::parse_date(c.lastTradeDateOrContractMonth)) {
            return model::option_symbol(
                {.root = c.tradingClass.empty() ? c.symbol : c.tradingClass,
                 .expiry = *expiry,
                 .type = option_type_of(c.right),
                 .strike = c.strike});
        }
    }
    if (!c.symbol.empty()) {
        return c.symbol; // an underlying: its ticker
    }
    return c.conId != 0 ? std::to_string(c.conId) : "UNKNOWN";
}

auto ib_contract(const std::string& symbol) -> Contract {
    Contract out;
    out.currency = "USD";
    out.exchange = "SMART";
    if (const auto id = model::parse_option_symbol(symbol)) {
        out.symbol = model::underlying_of(id->root);
        out.secType = "OPT";
        out.tradingClass = id->root;
        out.lastTradeDateOrContractMonth = std::format("{:%Y%m%d}", id->expiry);
        out.right = id->type == model::OptionType::CALL ? "C" : "P";
        out.strike = id->strike;
        return out;
    }
    out.symbol = symbol;
    const IbListing* listing = find_listing(symbol);
    out.secType = listing != nullptr ? listing->sec_type : "STK";
    if (listing != nullptr) {
        out.exchange = listing->exchange;
    }
    return out;
}

auto ib_combo_contract(std::span<const model::Leg> legs, const std::string& trading_class)
    -> Contract {
    Contract out;
    out.secType = "BAG";
    out.currency = "USD";
    out.exchange = "SMART";
    out.tradingClass = trading_class;
    out.comboLegs = std::make_shared<Contract::ComboLegList>();
    for (const model::Leg& leg : legs) {
        auto cl = std::make_shared<ComboLeg>();
        cl->conId = leg.con_id;
        cl->ratio = std::abs(leg.ratio);
        cl->action = (leg.side == model::Side::BUY) ? "BUY" : "SELL";
        cl->exchange = "SMART";
        out.comboLegs->push_back(cl);
        if (out.symbol.empty() && !leg.symbol.empty()) {
            const auto id = model::parse_option_symbol(leg.symbol);
            out.symbol = id ? model::underlying_of(id->root) : leg.symbol;
        }
    }
    return out;
}

auto underlying_contract(const ContractDetails& details) -> model::Contract {
    const Contract& c = details.contract;
    model::Contract out;
    out.symbol = platform_symbol(c);
    out.name = details.longName;
    out.product = product_of(c.secType);
    out.price_tick = details.minTick > 0.0 ? details.minTick : 0.01;
    out.con_id = c.conId;
    if (!c.tradingClass.empty()) {
        out.trading_class = c.tradingClass;
    }
    return out;
}

auto option_contract(const ContractDetails& details, const std::string& underlying_symbol)
    -> model::Contract {
    const Contract& c = details.contract;
    model::Contract out = underlying_contract(details);
    out.name = c.localSymbol;
    out.multiplier = 100.0;
    std::from_chars(c.multiplier.data(), c.multiplier.data() + c.multiplier.size(), out.multiplier);
    out.option_strike = c.strike;
    out.option_type = option_type_of(c.right);
    if (const auto date = model::parse_date(c.lastTradeDateOrContractMonth)) {
        out.option_expiry = model::option_expiry_time(*date);
    }
    out.option_underlying = underlying_symbol;
    return out;
}

auto expiries_within(const std::set<std::string>& expirations, std::chrono::year_month_day from,
                     int max_dte) -> std::vector<std::string> {
    const std::chrono::year_month_day to{std::chrono::sys_days{from} + std::chrono::days{max_dte}};
    const std::string first = std::format("{:%Y%m%d}", from);
    const std::string last = std::format("{:%Y%m%d}", to);
    std::vector<std::string> out;
    for (const std::string& e : expirations) { // a set: in order
        if (first <= e && e <= last) {
            out.push_back(e);
        }
    }
    return out;
}

// ---- Session
namespace {

using namespace std::chrono_literals;

constexpr auto kWake = 100ms; // the longest the message thread waits for a message
constexpr auto kRetry = 5s;   // between attempts to reconnect, and from a reconnect to the next

// About IB's own connections (data farms and the like), and the end of an account subscription
// the gateway asked for (2100): news, not errors.
constexpr std::array kNotices{1102, 2100, 2104, 2106, 2107, 2108, 2119, 2158};

/** How loud a message about the connection is: news, a warning (lost connectivity, 21xx) or an
 * error. */
auto connection_message_level(int code) -> utilities::LogLevel {
    if (std::ranges::contains(kNotices, code)) {
        return utilities::INFO;
    }
    if (code == 1100 || (code >= 2100 && code < 2200)) {
        return utilities::WARNING;
    }
    return utilities::ERROR;
}

} // namespace

IbSession::IbSession(const utilities::LogSource& owner)
    : owner_(owner), signal_(static_cast<unsigned long>(kWake.count())), socket_(this, &signal_) {}

IbSession::~IbSession() { stop(); }

auto IbSession::start(const std::string& host, int port, int client_id) -> bool {
    stop();
    host_ = host;
    port_ = port;
    client_id_ = client_id;
    reconnecting_ = false;
    if (!open()) {
        return false;
    }
    on_connected();
    thread_ = std::jthread([this](const std::stop_token& st) { run(st); });
    return true;
}

void IbSession::stop() {
    if (thread_.joinable()) {
        thread_.request_stop();
        thread_.join();
    }
    close();
    run_queued_jobs(); // not connected: their sends fail, and say so
}

void IbSession::queue_on_message_thread(std::function<void()> job) {
    jobs_.push(new Job{.run = std::move(job)}); // NOLINT(cppcoreguidelines-owning-memory)
    signal_.issueSignal(); // the message thread stops waiting for IB
}

void IbSession::run_queued_jobs() {
    // A job pushed meanwhile but not linked yet comes out at the next run: its signal wakes it.
    while (Job* job = jobs_.pop()) {
        const std::unique_ptr<Job> done(job);
        done->run();
    }
}

auto IbSession::open() -> bool {
    std::scoped_lock lock(send_mutex_);
    // TWS paces a burst of requests (over 50 a second) instead of dropping the connection.
    socket_.setConnectOptions("+PACEAPI");
    if (!socket_.eConnect(host_.c_str(), port_, client_id_)) {
        return false;
    }
    reader_ = std::make_unique<EReader>(&socket_, &signal_);
    reader_->start();
    connected_ = true;
    return true;
}

void IbSession::close() {
    std::scoped_lock lock(send_mutex_);
    connected_ = false;
    // The TWS API's reader registers with the socket and never unregisters, and the socket's
    // eDisconnect joins the registered reader's thread: unregistered first, the reader's destructor
    // joins its thread once, and the eDisconnect after it touches no freed reader.
    socket_.registerEReader(nullptr);
    reader_.reset();
    socket_.eDisconnect();
}

void IbSession::run(const std::stop_token& st) {
    std::chrono::steady_clock::time_point retry_at{};
    while (!st.stop_requested()) {
        if (connected_ && socket_.isConnected()) {
            signal_.waitForSignal();
            run_queued_jobs(); // before IB's messages: an order booked before its answers come
            reader_->processMsgs();
            run_queued_jobs();
            on_wake();
            continue;
        }
        run_queued_jobs(); // not connected: their sends fail, and say so
        if (!reconnecting_) {
            if (reader_) {
                reader_->processMsgs(); // what IB said before it closed, its reason among it
            }
            close();
            reconnecting_ = true;
            log(std::format("IB connection {}:{} lost; reconnecting", host_, port_), utilities::WARNING);
            on_disconnected();
        }
        const auto now = std::chrono::steady_clock::now();
        if (now < retry_at) {
            std::this_thread::sleep_for(kWake);
        } else if (open()) {
            reconnecting_ = false;
            retry_at = now + kRetry; // if it drops at once (IB Gateway restarting), the next waits
            log(std::format("IB connection {}:{} restored", host_, port_), utilities::INFO);
            on_connected();
            on_reconnected();
        } else {
            retry_at = now + kRetry;
        }
    }
}

void IbSession::error(int id, long long /*errorTimeMs*/, int errorCode,
                      const std::string& errorString,
                      const std::string& /*advancedOrderRejectJson*/) {
    if (id >= 0) { // about a request; -1 is about the connection
        on_error(id, errorCode, errorString);
        return;
    }
    if (errorCode == 502 && reconnecting_) {
        return; // another failed attempt to reconnect: run() said it is reconnecting
    }
    log(std::format("IB {}: {}", errorCode, errorString), connection_message_level(errorCode));
}

void IbSession::on_error(int id, int code, const std::string& message) {
    log(std::format("IB {} (request {}): {}", code, id, message), utilities::ERROR);
}

void IbSession::connectionClosed() {
    connected_ = false; // the message thread notices, says so and reconnects
}

} // namespace adapters
