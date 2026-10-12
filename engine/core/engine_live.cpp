/** The live assembly of core::Engine. */

#include "core/engine_live.hpp"
#include "strategy/strategy_registry.hpp"
#ifdef OPTRA_WITH_IB
#include "adapters/ib/engine_gateway_ib.hpp"
#include "adapters/ib/feed_ib.hpp"
#endif
#include <chrono>
#include <cstdlib>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace core::live {

namespace {

/** The build's source of quotes: IB with the IB gateway, else none. */
auto make_feed() -> std::unique_ptr<adapters::MarketDataFeed> {
#ifdef OPTRA_WITH_IB
    return std::make_unique<adapters::IbFeed>();
#else
    return nullptr;
#endif
}

} // namespace

auto make_gateway(utilities::Logger* logger) -> std::unique_ptr<adapters::Gateway> {
#ifdef OPTRA_WITH_IB
    return std::make_unique<adapters::IbGateway>(logger);
#else
    return std::make_unique<adapters::Gateway>(logger);
#endif
}

MainEngine::MainEngine(std::unique_ptr<adapters::Gateway> gateway) {
    clock_ = [] { return std::chrono::system_clock::now(); };
    assemble(gateway ? std::move(gateway) : make_gateway(log_engine()), make_feed());
}

MainEngine::MainEngine(const MakeGateway& make_gateway,
                       std::unique_ptr<adapters::MarketDataFeed> feed, Clock clock) {
    clock_ = std::move(clock);
    assemble(make_gateway(*portfolio_structure_), std::move(feed));
}

void MainEngine::assemble(std::unique_ptr<adapters::Gateway> gateway,
                          std::unique_ptr<adapters::MarketDataFeed> feed) {
    event_engine_ = std::make_unique<EventEngine>(this);
    event_engine_->start();
    db_engine_ = std::make_unique<adapters::DatabaseEngine>(log_engine());
    gateway_ = std::move(gateway);
    init_core(utilities::INFO);

    // The portfolios, from the contracts in the database. Days to expiry count from today until
    // the first snapshot moves them on (the runtime keeps the clock; the core reads none).
    portfolio_structure_->ensure_portfolios_created();
    db_engine_->load_contracts(
        [this](const model::ContractData& c) { portfolio_structure_->process_option(c); },
        [this](const model::ContractData& c) { portfolio_structure_->process_underlying(c); });
    portfolio_structure_->finalize_all_chains();
    const auto today = std::chrono::floor<std::chrono::days>(now());
    for (const std::string& name : portfolio_structure_->get_all_portfolio_names()) {
        portfolio_structure_->get_portfolio(name)->set_dte_ref(today);
    }

    // The market data's snapshots go to the event thread, the newest of each portfolio kept; the
    // venue's orders and fills join its queue (init_core).
    market_data_ = std::make_unique<adapters::MarketDataEngine>(
        log_engine(), *portfolio_structure_, std::move(feed),
        [this](const model::PortfolioSnapshot& s) { put_snapshot(s); });
    // OPTRA_RECORD_DIR: the quotes recorded a minute at a time, for a backtest of the same day.
    if (const char* dir = std::getenv("OPTRA_RECORD_DIR"); dir != nullptr && *dir != '\0') {
        market_data_->record_to(dir);
    }

    if (const char* mode = std::getenv("OPTRA_RECONCILE");
        mode != nullptr && std::string_view(mode) == "warn") {
        reconcile_warn_only_ = true;
        write_log("OPTRA_RECONCILE=warn: strategies start even while the account's positions"
                  " disagree with the holdings",
                  utilities::WARNING);
    }
    log_self_check();
    on_event_thread([this] { restore(); });
    write_log("Main engine initialization successful", utilities::INFO);
}

MainEngine::~MainEngine() { close(); }

void MainEngine::log_self_check() {
    auto classes = strategy::StrategyRegistry::get_all_strategy_class_names();
    write_log(std::format("Registered strategy classes: {}", classes.size()), utilities::INFO);
    for (const std::string& name : get_all_portfolio_names()) {
        model::PortfolioData* p = get_portfolio(name);
        if (p == nullptr) {
            continue;
        }
        std::string underlying_str = (p->underlying != nullptr) ? p->underlying->symbol : "None";
        const model::UnderlyingData* hedge = p->hedge();
        write_log(std::format("{} (underlying: {}, hedged with: {})", p->name, underlying_str,
                              hedge != nullptr ? hedge->symbol : "nothing"),
                  utilities::INFO);
        write_log("  chains: " + std::to_string(p->chains.size()), utilities::INFO);
        write_log("  options: " + std::to_string(p->option_apply_order().size()), utilities::INFO);
    }
}

// ---- The market data ----

void MainEngine::start_market_data_update() { market_data_->start(); }

void MainEngine::stop_market_data_update() { market_data_->stop(); }

// ---- After a restart ----

void MainEngine::resume() {
    try {
        connect();
    } catch (const std::exception& e) {
        write_log(std::format("Resume: cannot connect ({}); nothing started", e.what()),
                  utilities::ERROR);
        return;
    }
    try {
        start_market_data_update();
    } catch (const std::exception& e) {
        write_log(std::format("Resume: market data not started: {}", e.what()), utilities::ERROR);
    }
    for (const adapters::StrategyRecord& r : db_engine_->load_strategies()) {
        if (r.state != adapters::StrategyState::Running ||
            !on_event_thread([&] { return option_strategy_engine()->get_strategy(r.name); })) {
            continue;
        }
        try {
            start_strategy(r.name);
            write_log("Resume: started " + r.name, utilities::INFO);
        } catch (const std::exception& e) {
            write_log(std::format("Resume: {} not started: {}", r.name, e.what()),
                      utilities::ERROR);
        }
    }
}

} // namespace core::live
