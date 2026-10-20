/** The live assembly of core::Main, and its trading days. */

#include "assembly/live.hpp"
#include "adapters/database/sqlite_database.hpp"
#include "model/utility.hpp"
#include "strategy/strategy_registry.hpp"
#ifdef OPTRA_WITH_IB
#include "adapters/gateway/ib_gateway.hpp"
#include "adapters/market_data/ib_market_data.hpp"
#include <nlohmann/json.hpp>
#endif
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <format>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace live {

namespace {

/** The build's source of quotes: IB with the IB gateway, else none. */
auto make_market_data() -> std::unique_ptr<core::MarketData> {
#ifdef OPTRA_WITH_IB
    return std::make_unique<adapters::IbMarketData>();
#else
    return nullptr;
#endif
}

void log_self_check(core::Main& main) {
    auto classes = strategy::StrategyRegistry::get_all_strategy_class_names();
    main.write_log(std::format("Registered strategy classes: {}", classes.size()),
                   utilities::INFO);
    const model::Portfolios& structure = *main.portfolios();
    for (const std::string& name : structure.portfolio_names()) {
        const model::Portfolio* p = structure.find_portfolio(name);
        if (p == nullptr) {
            continue;
        }
        const std::optional<model::Underlying> underlying = p->underlying();
        const std::optional<model::Underlying> hedge = p->hedge();
        main.write_log(std::format("{} (underlying: {}, hedged with: {})", p->name,
                                   underlying ? underlying->symbol() : "None",
                                   hedge ? hedge->symbol() : "nothing"),
                       utilities::INFO);
        main.write_log("  chains: " + std::to_string(p->listing().chains().size()),
                       utilities::INFO);
        main.write_log("  options: " + std::to_string(p->listing().size()), utilities::INFO);
    }
}

/** Portfolios from stored contracts. DTE counts from today until the first snapshot. */
void load_saved_contracts(core::Main& main) {
    model::Portfolios& structure = *main.portfolios();
    structure.ensure_portfolios_created();
    const std::vector<model::Contract> options = main.database()->load_option_contracts();
    const std::vector<model::Contract> equities = main.database()->load_equity_contracts();
    std::vector<model::Contract> contracts = equities;
    contracts.insert(contracts.end(), options.begin(), options.end());
    structure.load_contracts(contracts);
    main.write_log(std::format("Loaded {} contracts", options.size() + equities.size()),
                   utilities::INFO);
    const auto today = std::chrono::floor<std::chrono::days>(main.now());
    for (const std::string& name : structure.portfolio_names()) {
        structure.find_portfolio(name)->set_trading_day(today);
    }
    main.market_data()->load_contracts();
}

#ifdef OPTRA_WITH_IB
/** What the class's library was built from: its .code "strategy"; empty for a class built into
 * the program, or a library without one. */
auto strategy_code(const std::string& class_name) -> std::string {
    const std::string library = strategy::StrategyRegistry::library(class_name);
    if (library.empty()) {
        return {};
    }
    std::ifstream in(library + ".code");
    try {
        return nlohmann::json::parse(in).value("strategy", std::string{});
    } catch (const std::exception&) {
        return {};
    }
}

/** A live account's approved strategies (IbSettings::live_strategies) as a test of a strategy:
 * its class, its library's code and its resolved setting all those of a line. A line that does
 * not read is logged and approves nothing. */
auto approved_by(const std::vector<std::string>& lines, core::Main& main)
    -> std::function<bool(const model::StrategyRecord&)> {
    struct Approved {
        std::string class_name;
        std::string code;
        std::unordered_map<std::string, double> setting; // the rest at the class's defaults
    };
    std::vector<Approved> approved;
    for (const std::string& line : lines) {
        std::istringstream in(line);
        Approved a;
        in >> a.class_name >> a.code;
        bool read = !a.class_name.empty() && !a.code.empty();
        for (std::string pair; read && in >> pair;) {
            const auto eq = pair.find('=');
            try {
                read = eq != std::string::npos && eq > 0;
                if (read) {
                    a.setting[pair.substr(0, eq)] = std::stod(pair.substr(eq + 1));
                }
            } catch (const std::exception&) {
                read = false;
            }
        }
        if (!read) {
            main.write_log("Approved strategies: a line that does not read, left out: " + line,
                           utilities::ERROR);
            continue;
        }
        approved.push_back(std::move(a));
    }
    return [approved = std::move(approved)](const model::StrategyRecord& record) {
        const std::string code = strategy_code(record.class_name);
        return !code.empty() && std::ranges::any_of(approved, [&](const Approved& a) {
            if (a.class_name != record.class_name || a.code != code) {
                return false;
            }
            try {
                return strategy::StrategyRegistry::resolve_setting(a.class_name, a.setting) ==
                       record.setting;
            } catch (const std::exception&) {
                return false;
            }
        });
    };
}
#endif

} // namespace

auto make_gateway(utilities::Logger* logger) -> std::unique_ptr<core::Gateway> {
#ifdef OPTRA_WITH_IB
    return std::make_unique<adapters::IbGateway>(logger);
#else
    return std::make_unique<core::Gateway>(logger);
#endif
}

auto make_main(std::unique_ptr<core::Gateway> gateway) -> std::unique_ptr<core::Main> {
    // std::function must be copyable, so the unique_ptr goes through a shared holder.
    auto given = std::make_shared<std::unique_ptr<core::Gateway>>(std::move(gateway));
    return make_main(
        [given](utilities::Logger* logger, const model::Portfolios&) {
            return *given ? std::move(*given) : make_gateway(logger);
        },
        make_market_data(), [] { return std::chrono::system_clock::now(); });
}

auto make_main(const MakeGateway& make_gateway, std::unique_ptr<core::MarketData> market_data,
               Clock clock) -> std::unique_ptr<core::Main> {
    const char* check = std::getenv("OPTRA_BROKER_CHECK");
    const bool warn_only = check != nullptr && std::string_view(check) == "warn";
    auto main = std::make_unique<core::Main>(core::Main::Ports{
        .clock = std::move(clock),
        .database = [](utilities::Logger* logger) -> std::unique_ptr<core::Database> {
            return std::make_unique<adapters::SqliteDatabase>(logger);
        },
        .gateway = make_gateway,
        .market_data = std::move(market_data),
        .broker_check_warn_only = warn_only,
        .daily = [] {
            const char* dir = std::getenv("OPTRA_DAILY_DIR");
            return model::DailyTable::load(dir != nullptr && *dir != '\0' ? dir : "data/daily");
        }});
    main->event_loop()->configure_from_environment(); // OPTRA_EVENT_SPIN_US, OPTRA_EVENT_CPU
    main->event_loop()->start();
    std::chrono::microseconds budget = core::StrategyEngine::kLiveCallbackBudget;
    if (const char* us = std::getenv("OPTRA_CALLBACK_BUDGET_US"); us != nullptr && *us != '\0') {
        budget = std::chrono::microseconds(std::strtol(us, nullptr, 10));
    }
    main->strategy_engine()->set_callback_budget(budget);
    load_saved_contracts(*main);
    if (warn_only) {
        main->write_log("OPTRA_BROKER_CHECK=warn: strategies start even while the account's"
                        " positions disagree with the holdings",
                        utilities::WARNING);
    }
    log_self_check(*main);
#ifdef OPTRA_WITH_IB
    // A live account's engine runs only what root approved, saved strategies included.
    if (const auto* ib = dynamic_cast<const adapters::IbGateway*>(main->gateway());
        ib != nullptr && ib->default_setting().is_live_account()) {
        const adapters::IbSettings& settings = ib->default_setting();
        const std::vector<std::string> lines = settings.live_strategies();
        main->event_loop()->on_event_thread([&main, &lines] {
            main->strategy_engine()->approve_only(approved_by(lines, *main));
        });
        main->write_log(std::format("A live account: only the {} strategies approved in {} run",
                                    lines.size(), settings.live_strategies_file),
                        utilities::WARNING);
    }
#endif
    main->event_loop()->on_event_thread([&main] { main->restore(); });
    main->write_log("Main engine initialization successful", utilities::INFO);
    return main;
}

// ---- The trading days ----

void TradingDays::start(std::vector<Synced> portfolios, bool resume) {
    if (started_) {
        return;
    }
    started_ = true;
    const bool begun = begin_day(portfolios, resume);
    thread_ = std::jthread([this, portfolios = std::move(portfolios), resume, begun](
                               std::stop_token stop) {
        run_days(std::move(stop), portfolios, resume, begun);
    });
}

void TradingDays::stop() {
    if (thread_.joinable()) {
        thread_.request_stop();
        interrupt(); // a roll under way fails, all or nothing, and the thread wakes to its stop
        thread_.join();
    }
    main_.market_data()->stop();
    main_.gateway()->disconnect();
}

void TradingDays::interrupt() { main_.gateway()->disconnect(); }

auto TradingDays::connected() -> bool {
    if (main_.gateway()->is_connected()) {
        return true;
    }
    try {
        main_.connect();
    } catch (const std::exception& e) {
        if (!connect_failed_) {
            main_.write_log(std::format("Cannot connect ({}): trying again each minute",
                                        e.what()),
                            utilities::ERROR);
        }
        connect_failed_ = true;
        return false;
    }
    if (connect_failed_) {
        main_.write_log("Connected to the broker again", utilities::INFO);
    }
    connect_failed_ = false;
    return true;
}

auto TradingDays::begin_day(std::span<const Synced> portfolios, bool resume) -> bool {
    if (!connected()) {
        return false;
    }
    if (!portfolios.empty()) {
        try {
            main_.roll_day(portfolios);
        } catch (const std::exception& e) {
            main_.write_log(std::format("The day's contracts did not come in ({}): nothing starts "
                                        "until they do (tried again each minute)",
                                        e.what()),
                            utilities::ERROR);
            return false;
        }
    }
    try {
        main_.market_data()->start();
    } catch (const std::exception& e) {
        main_.write_log(std::format("Market data not started: {}", e.what()), utilities::ERROR);
    }
    if (resume) {
        main_.start_strategies_that_were_running();
    }
    return true;
}

void TradingDays::run_days(std::stop_token stop, std::vector<Synced> portfolios, bool resume,
                           bool begun) {
    using namespace std::chrono;
    std::mutex sleeping;
    std::unique_lock lock(sleeping);
    std::condition_variable_any wake; // never notified: waits for the time or the stop
    // By the engine's clock: a simulated market's lags the wall clock.
    const auto sleep_until = [&](model::DateTime at) {
        wake.wait_for(lock, stop, at - main_.now(), [] { return false; });
        return !stop.stop_requested();
    };
    // Retry the first day each retry_ (a minute) if start() could not begin it.
    while (!begun) {
        if (!sleep_until(main_.now() + retry_)) {
            return;
        }
        begun = begin_day(portfolios, resume);
    }
    if (portfolios.empty()) {
        return; // nothing to roll: the contracts saved are the engine's
    }
    // Each day at the earliest roll of the portfolios' calendars, on that calendar's days (a
    // calendar that never rolls, a market open around the clock, only syncs, above); a failed
    // sync retries each minute on yesterday's contracts.
    const model::SessionCalendar* calendar = nullptr;
    for (const Synced& s : portfolios) {
        const model::SessionCalendar& c = model::calendar_of(s.first);
        if (c.roll && (calendar == nullptr || *c.roll < *calendar->roll)) {
            calendar = &c;
        }
    }
    if (calendar == nullptr) {
        return;
    }
    const minutes roll = *calendar->roll;
    year_month_day day = calendar->date_of(main_.now());
    if (!calendar->is_trading_day(day) || main_.now() >= calendar->at(day, roll)) {
        day = calendar->next_trading_day(day);
    }
    while (sleep_until(calendar->at(day, roll))) {
        try {
            if (!connected()) {
                throw std::runtime_error("not connected to the broker");
            }
            main_.roll_day(portfolios);
            day = calendar->next_trading_day(day);
        } catch (const std::exception& e) {
            main_.write_log(std::format("The day's contracts did not come in ({}): trying again "
                                        "in a minute",
                                        e.what()),
                            utilities::ERROR);
            if (!sleep_until(main_.now() + retry_)) {
                return;
            }
        }
    }
}

} // namespace live
