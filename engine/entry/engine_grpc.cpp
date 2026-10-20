#include "entry/engine_grpc.hpp"
#include "core/database/database.hpp"
#include "model/event.hpp"
#include "utilities/log_level.hpp"
#include "model/portfolio.hpp"
#include "model/symbol.hpp"
#include "model/utility.hpp"
#include "strategy/strategy_registry.hpp"

#include <cctype>
#include <optional>
#include <memory>
#include <functional>
#include <thread>
#include <print>
#include <mutex>
#include <ctime>
#include <cstdlib>
#include <csignal>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <nlohmann/json.hpp>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace entry {

namespace {


/** A call's exception as a status: a bad request; IB not answering in time; the connection to
 * IB dropped; else a state the engine cannot do it in. */
auto failed(const std::exception& e) -> ::grpc::Status {
    if (dynamic_cast<const std::invalid_argument*>(&e) != nullptr) {
        return {::grpc::StatusCode::INVALID_ARGUMENT, e.what()};
    }
    if (const auto* error = dynamic_cast<const std::system_error*>(&e); error != nullptr) {
        if (error->code() == std::errc::timed_out) {
            return {::grpc::StatusCode::DEADLINE_EXCEEDED, e.what()}; // IB did not answer in time
        }
        if (error->code() == std::errc::not_connected) {
            return {::grpc::StatusCode::UNAVAILABLE, e.what()}; // the connection to IB dropped
        }
    }
    return {::grpc::StatusCode::FAILED_PRECONDITION, e.what()};
}

void diff_msg(const core::BrokerCheckRow& c, ::optra::PositionDiffMsg* out) {
    out->set_symbol(c.symbol);
    out->set_engine(c.engine);
    out->set_broker(c.broker);
}

/** The status of running fn: OK, or what it threw (failed). */
template <typename Fn> auto status_of(Fn&& fn) -> ::grpc::Status {
    try {
        std::forward<Fn>(fn)();
        return ::grpc::Status::OK;
    } catch (const std::exception& e) {
        return failed(e);
    }
}


/** PositionMsg and OptionPositionMsg share these fields. */
template <typename Msg> void position_msg(const model::Position& pos, Msg* msg) {
    msg->set_symbol(pos.symbol);
    msg->set_quantity(pos.quantity);
    msg->set_avg_cost(pos.avg_cost);
    msg->set_cost_value(pos.cost_value);
    msg->set_realized_pnl(pos.realized_pnl);
    msg->set_mid_price(pos.mid_price);
    msg->set_delta(pos.delta);
    msg->set_gamma(pos.gamma);
    msg->set_theta(pos.theta);
    msg->set_vega(pos.vega);
    msg->set_multiplier(pos.multiplier);
}

/** The holding as the API sends it (StrategyHoldingMsg; a combo's type by its enum name). */
auto holding_msg(const model::StrategyHolding& holding) -> ::optra::StrategyHoldingMsg {
    ::optra::StrategyHoldingMsg msg;
    position_msg(holding.shares, msg.mutable_shares());
    for (const auto* options : {&holding.options, &holding.closed_options}) {
        for (const auto& [symbol, option] : *options) {
            ::optra::OptionPositionMsg& m = (*msg.mutable_options())[symbol];
            position_msg(option, &m);
            m.set_combo(option.combo);
            m.set_combo_type(model::to_string(option.combo_type));
        }
    }
    const model::HoldingSummary& s = holding.summary;
    ::optra::HoldingSummaryMsg* sm = msg.mutable_summary();
    sm->set_total_cost(s.total_cost);
    sm->set_current_value(s.current_value);
    sm->set_unrealized_pnl(s.unrealized_pnl);
    sm->set_realized_pnl(s.realized_pnl);
    sm->set_pnl(s.pnl);
    sm->set_delta(s.delta);
    sm->set_gamma(s.gamma);
    sm->set_theta(s.theta);
    sm->set_vega(s.vega);
    sm->set_pnl_delta(s.pnl_delta);
    sm->set_pnl_gamma(s.pnl_gamma);
    sm->set_pnl_theta(s.pnl_theta);
    sm->set_pnl_vega(s.pnl_vega);
    sm->set_pnl_other(s.pnl_other);
    sm->set_pnl_fills(s.pnl_fills);
    return msg;
}

} // namespace

namespace {

/** The trading core this program was built from, as its build wrote it beside it (code_identity.cmake);
 * empty when it did not. Read at start: a rebuild while it runs is not its code. */
auto built_code() -> std::string {
    std::error_code error;
    const std::filesystem::path program = std::filesystem::read_symlink("/proc/self/exe", error);
    std::ifstream in(program.string() + ".code");
    if (error || !in) {
        return {};
    }
    const nlohmann::json code = nlohmann::json::parse(in, nullptr, /*allow_exceptions=*/false);
    return code.is_object() ? code.value("engine", std::string{}) : std::string{};
}

} // namespace

GrpcLiveEngineService::GrpcLiveEngineService(core::Main& engine)
    : engine_(engine), code_(built_code()) {}

auto GrpcLiveEngineService::GetStatus(::grpc::ServerContext* /*context*/,
                                      const ::optra::Empty* /*request*/,
                                      ::optra::EngineStatus* response) -> ::grpc::Status {
    const bool connected = engine_.gateway()->is_connected();
    const bool md_running = engine_.market_data()->running();
    const bool halted =
        engine_.read([](const core::Main& m) { return m.risk_engine()->halted(); });
    response->set_running(true);
    response->set_connected(connected);
    response->set_halted(halted);
    response->set_code(code_);
    response->set_detail(std::format("engine: running; ib: {}; md: {}{}", connected ? "on" : "off",
                                     md_running ? "on" : "off", halted ? "; trading: halted" : ""));
    return ::grpc::Status::OK;
}

auto GrpcLiveEngineService::ListStrategies(::grpc::ServerContext* /*context*/,
                                           const ::optra::Empty* /*request*/,
                                           ::grpc::ServerWriter<::optra::StrategySummary>* writer)
    -> ::grpc::Status {
    return status_of([&] {
        const auto records = engine_.read([](const core::Main& m) {
            return std::vector<model::StrategyRecord>(std::from_range,
                                                      m.strategy_engine()->strategies());
        });
        for (const model::StrategyRecord& r : records) {
            ::optra::StrategySummary sum;
            sum.set_strategy_name(r.name);
            sum.set_class_name(r.class_name);
            sum.set_portfolio(r.portfolio);
            sum.set_status(model::to_string(r.state)); // added, inited, running or error
            sum.mutable_settings()->insert(r.setting.begin(), r.setting.end());
            writer->Write(sum);
        }
    });
}

auto GrpcLiveEngineService::ConnectGateway(::grpc::ServerContext* /*context*/,
                                           const ::optra::Empty* /*request*/,
                                           ::optra::Empty* /*response*/) -> ::grpc::Status {
    return status_of([&] { engine_.connect(); });
}

auto GrpcLiveEngineService::DisconnectGateway(::grpc::ServerContext* /*context*/,
                                              const ::optra::Empty* /*request*/,
                                              ::optra::Empty* /*response*/) -> ::grpc::Status {
    return status_of([&] { engine_.gateway()->disconnect(); });
}

auto GrpcLiveEngineService::StartMarketData(::grpc::ServerContext* /*context*/,
                                            const ::optra::Empty* /*request*/,
                                            ::optra::Empty* /*response*/) -> ::grpc::Status {
    return status_of([&] { engine_.market_data()->start(); });
}

auto GrpcLiveEngineService::StopMarketData(::grpc::ServerContext* /*context*/,
                                           const ::optra::Empty* /*request*/,
                                           ::optra::Empty* /*response*/) -> ::grpc::Status {
    return status_of([&] { engine_.market_data()->stop(); });
}

auto GrpcLiveEngineService::StartStrategy(::grpc::ServerContext* /*context*/,
                                          const ::optra::StrategyNameRequest* request,
                                          ::optra::Empty* /*response*/) -> ::grpc::Status {
    return status_of([&] { engine_.start_strategy(request->strategy_name()); });
}

auto GrpcLiveEngineService::StopStrategy(::grpc::ServerContext* /*context*/,
                                         const ::optra::StrategyNameRequest* request,
                                         ::optra::Empty* /*response*/) -> ::grpc::Status {
    return status_of([&] { engine_.stop_strategy(request->strategy_name()); });
}

auto GrpcLiveEngineService::StreamLogs(::grpc::ServerContext* context,
                                       const ::optra::Empty* /*request*/,
                                       ::grpc::ServerWriter<::optra::LogLine>* writer)
    -> ::grpc::Status {
    const std::uint64_t subscription = ++log_subscription_;
    while (!context->IsCancelled() && log_subscription_ == subscription) {
        utilities::LogRecord log;
        if (!engine_.log_engine()->pop_log_for_stream(log, 1000)) {
            continue;
        }
        const nlohmann::json line{
            {"src", "live"},
            {"time", log.time},
            {"level", std::to_underlying(log.level)},
            {"level_str", utilities::level_to_string(log.level)},
            {"source", log.source},
            {"msg", log.msg},
        };
        ::optra::LogLine msg;
        // Invalid UTF-8 (a broker's message, say) is replaced rather than thrown on.
        msg.set_line(line.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
        if (!writer->Write(msg)) {
            break;
        }
    }
    return ::grpc::Status::OK;
}

auto GrpcLiveEngineService::GetOrdersAndFills(::grpc::ServerContext* /*context*/,
                                              const ::optra::OrdersAndFillsRequest* request,
                                              ::optra::OrdersAndFillsResponse* response)
    -> ::grpc::Status {
    return status_of([&] {
        if (request->limit() < 0) {
            throw std::invalid_argument("limit is negative");
        }
        const auto limit = static_cast<std::size_t>(request->limit());
        core::Database& database = *engine_.database();
        const std::vector<std::string> strategies =
            request->strategy_name().empty() ? std::vector<std::string>{} // all
                                             : std::vector{request->strategy_name()};
        // As the tables keep them: the text the database writes.
        for (const core::SavedOrder& saved : database.load_orders(strategies, limit)) {
            const model::Order& o = saved.record;
            ::optra::OrderRecord* r = response->add_orders();
            r->set_timestamp(model::datetime_to_str(saved.saved_at));
            r->set_strategy_name(saved.strategy_name);
            r->set_order_id(o.order_id);
            r->set_symbol(o.symbol);
            r->set_trading_class(o.trading_class);
            r->set_type(model::to_string(o.type));
            // A contract's own order's side; a combo's none (its legs say).
            r->set_side(o.is_combo() || o.legs.empty()
                                 ? std::string{}
                                 : model::to_string(o.legs.front().side));
            r->set_price(o.price);
            r->set_quantity(o.quantity);
            r->set_filled(o.filled);
            r->set_status(model::to_string(o.status));
            r->set_datetime(o.datetime != model::DateTime{} ? model::datetime_to_str(o.datetime)
                                                            : "N/A");
            r->set_reference(o.reference);
            r->set_is_combo(o.is_combo());
        }
        for (const core::SavedFill& saved : database.load_fills(strategies, limit)) {
            const model::Fill& t = saved.record;
            ::optra::FillRecord* r = response->add_fills();
            r->set_timestamp(model::datetime_to_str(saved.saved_at));
            r->set_strategy_name(saved.strategy_name);
            r->set_fill_id(t.fill_id);
            r->set_symbol(t.symbol);
            r->set_order_id(t.order_id);
            r->set_side(model::to_string(t.side));
            r->set_price(t.price);
            r->set_quantity(t.quantity);
            r->set_datetime(t.datetime != model::DateTime{} ? model::datetime_to_str(t.datetime)
                                                            : "");
        }
        for (const std::string& name : database.strategies_with_records()) {
            response->add_strategies(name);
        }
    });
}

auto GrpcLiveEngineService::ListSavedContracts(::grpc::ServerContext* /*context*/,
                                               const ::optra::Empty* /*request*/,
                                               ::optra::SavedContractsResponse* response)
    -> ::grpc::Status {
    return status_of([&] {
        std::vector<model::Contract> contracts = engine_.database()->load_option_contracts();
        contracts.append_range(engine_.database()->load_equity_contracts());
        for (const model::Contract& c : contracts) {
            ::optra::ContractMsg* m = response->add_contracts();
            m->set_symbol(c.symbol);
            m->set_product(model::to_string(c.product));
            m->set_multiplier(c.multiplier);
            m->set_price_tick(c.price_tick);
            if (c.option_type) {
                m->set_option_type(model::to_string(*c.option_type));
            }
            m->set_option_strike(c.option_strike.value_or(0.0));
            if (c.option_expiry) {
                m->set_option_expiry(
                    std::format("{:%F}", model::calendar_of(c.trading_class)
                                             .date_of(*c.option_expiry)));
            }
            m->set_option_underlying(c.option_underlying.value_or(""));
        }
    });
}

auto GrpcLiveEngineService::WipeDatabase(::grpc::ServerContext* /*context*/,
                                         const ::optra::WipeDatabaseRequest* request,
                                         ::optra::WipeDatabaseResponse* response)
    -> ::grpc::Status {
    return status_of([&] {
        for (const auto& [table, rows] : engine_.wipe_database(request->what())) {
            (*response->mutable_emptied())[table] = rows;
        }
    });
}

auto GrpcLiveEngineService::ListPortfolios(::grpc::ServerContext* /*context*/,
                                           const ::optra::Empty* /*request*/,
                                           ::optra::ListPortfoliosResponse* response)
    -> ::grpc::Status {
    return status_of([&] {
        for (const std::string& name : engine_.portfolios()->portfolio_names()) {
            response->add_portfolios(name);
        }
    });
}

auto GrpcLiveEngineService::GetQuotes(::grpc::ServerContext* /*context*/,
                                      const ::optra::QuotesRequest* request,
                                      ::optra::QuotesResponse* response) -> ::grpc::Status {
    return status_of([&] {
        // Only the quotes copied on the event thread (the listing is immutable, shared); the
        // symbols and the response made off it.
        model::DateTime time{};
        double underlying_bid = 0.0;
        double underlying_ask = 0.0;
        std::shared_ptr<const model::Listing> listing;
        std::vector<double> bids;
        std::vector<double> asks;
        engine_.read([&](const core::Main& main) {
            const model::Portfolio* p = main.find_portfolio(request->portfolio());
            if (p == nullptr) {
                throw std::invalid_argument("No portfolio " + request->portfolio());
            }
            time = p->datetime();
            if (const std::optional<model::Underlying> u = p->underlying()) {
                underlying_bid = u->bid() > 0.0 ? u->bid() : u->mid();
                underlying_ask = u->ask() > 0.0 ? u->ask() : u->mid();
            }
            listing = p->shared_listing();
            bids.assign(p->frame().bid.begin(), p->frame().bid.end());
            asks.assign(p->frame().ask.begin(), p->frame().ask.end());
        });
        const auto ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch());
        response->set_time_ns(time == model::DateTime{} ? 0 : ns.count());
        response->set_underlying_bid(underlying_bid);
        response->set_underlying_ask(underlying_ask);
        const auto at = [](const std::vector<double>& v, std::size_t i) {
            return i < v.size() ? v[i] : 0.0;
        };
        for (std::size_t column = 0; listing != nullptr && column < listing->size(); ++column) {
            const double bid = at(bids, column);
            const double ask = at(asks, column);
            if (bid <= 0.0 && ask <= 0.0) {
                continue;
            }
            if (const std::optional<model::OptionId> id =
                    model::parse_option_symbol(listing->contract(column).symbol)) {
                ::optra::OptionQuote* m = response->add_options();
                m->set_symbol(model::occ_code(*id));
                m->set_bid(bid);
                m->set_ask(ask);
            }
        }
    });
}

auto GrpcLiveEngineService::SyncContracts(::grpc::ServerContext* /*context*/,
                                          const ::optra::SyncContractsRequest* request,
                                          ::optra::SyncContractsResponse* response)
    -> ::grpc::Status {
    return status_of([&] {
        int options = 0;
        std::set<std::string> expiries;
        const std::vector<live::Synced> portfolio{{request->portfolio(), request->max_dte()}};
        for (const model::Contract& c : engine_.roll_day(portfolio)) { // traded at once
            if (c.product == model::Product::OPTION && c.option_expiry) {
                ++options;
                expiries.insert(std::format(
                    "{:%F}", model::calendar_of(c.trading_class).date_of(*c.option_expiry)));
            }
        }
        response->set_options(options);
        for (const std::string& e : expiries) {
            response->add_expiries(e);
        }
    });
}

auto GrpcLiveEngineService::ListStrategyClasses(::grpc::ServerContext* /*context*/,
                                                const ::optra::Empty* /*request*/,
                                                ::optra::ListStrategyClassesResponse* response)
    -> ::grpc::Status {
    return status_of([&] {
        strategy::StrategyRegistry::reload(); // newly built libraries
        for (const std::string& c : strategy::StrategyRegistry::get_all_strategy_class_names()) {
            response->add_classes(c);
        }
        for (const strategy::StrategyRegistry::Refusal& r : strategy::StrategyRegistry::refused()) {
            response->add_refused(r.library + ": " + r.why);
        }
    });
}

auto GrpcLiveEngineService::GetRemovedStrategies(::grpc::ServerContext* /*context*/,
                                                 const ::optra::Empty* /*request*/,
                                                 ::optra::GetRemovedStrategiesResponse* response)
    -> ::grpc::Status {
    return status_of([&] {
        for (const model::StrategyRecord& r : engine_.database()->load_strategies()) {
            if (r.state == model::StrategyState::Removed) {
                response->add_removed_strategies(r.name);
            }
        }
    });
}

auto GrpcLiveEngineService::AddStrategy(::grpc::ServerContext* /*context*/,
                                        const ::optra::AddStrategyRequest* request,
                                        ::optra::AddStrategyResponse* response)
    -> ::grpc::Status {
    return status_of([&] {
        response->set_strategy_name(engine_.add_strategy(
            request->strategy_class(), request->portfolio_name(),
            std::unordered_map<std::string, double>(request->setting().begin(),
                                                    request->setting().end()),
            request->strategy_name()));
    });
}

auto GrpcLiveEngineService::RestoreStrategy(::grpc::ServerContext* /*context*/,
                                            const ::optra::StrategyNameRequest* request,
                                            ::optra::Empty* /*response*/) -> ::grpc::Status {
    return status_of([&] { engine_.restore_strategy(request->strategy_name()); });
}

auto GrpcLiveEngineService::InitStrategy(::grpc::ServerContext* /*context*/,
                                         const ::optra::StrategyNameRequest* request,
                                         ::optra::Empty* /*response*/) -> ::grpc::Status {
    return status_of([&] { engine_.init_strategy(request->strategy_name()); });
}

auto GrpcLiveEngineService::RemoveStrategy(::grpc::ServerContext* /*context*/,
                                           const ::optra::StrategyNameRequest* request,
                                           ::optra::RemoveStrategyResponse* response)
    -> ::grpc::Status {
    return status_of([&] {
        engine_.remove_strategy(request->strategy_name());
        response->set_removed(true);
    });
}

auto GrpcLiveEngineService::DeleteStrategy(::grpc::ServerContext* /*context*/,
                                           const ::optra::StrategyNameRequest* request,
                                           ::optra::DeleteStrategyResponse* response)
    -> ::grpc::Status {
    return status_of([&] {
        engine_.delete_strategy(request->strategy_name());
        response->set_deleted(true);
    });
}

auto GrpcLiveEngineService::GetStrategyHoldings(::grpc::ServerContext* /*context*/,
                                                const ::optra::Empty* /*request*/,
                                                ::optra::StrategyHoldingsResponse* response)
    -> ::grpc::Status {
    return status_of([&] {
        const auto holdings = engine_.read([](const core::Main& m) {
            std::vector<std::pair<std::string, model::StrategyHolding>> out;
            for (const model::StrategyRecord& r : m.strategy_engine()->strategies()) {
                if (const model::StrategyHolding* h = m.bookkeeping_engine()->find_holding(r.name)) {
                    out.emplace_back(r.name, *h);
                }
            }
            return out;
        });
        for (const auto& [name, holding] : holdings) {
            (*response->mutable_holdings())[name] = holding_msg(holding);
        }
    });
}

auto GrpcLiveEngineService::GetAccount(::grpc::ServerContext* /*context*/,
                                       const ::optra::Empty* /*request*/,
                                       ::optra::AccountResponse* response) -> ::grpc::Status {
    return status_of([&] {
        // IB's side when it can be asked; the engine's side always.
        core::BrokerAccount account;
        try {
            account = engine_.gateway()->query_account().get();
            response->set_from_ib(true);
        } catch (const std::exception& e) {
            response->set_ib_error(e.what());
        }
        response->set_account(account.account);
        response->set_updated(account.updated);
        for (const core::AccountValue& v : account.values) {
            ::optra::AccountValueMsg* m = response->add_values();
            m->set_key(v.key);
            m->set_value(v.value);
            m->set_currency(v.currency);
        }
        // Each contract held here or at IB, with what IB says it is worth.
        std::map<std::string, const core::AccountPosition*> valued;
        for (const core::AccountPosition& p : account.positions) {
            valued[p.symbol] = &p;
        }
        // Read only: the check strategies start on is recorded by the connect and by CheckWithBroker.
        const std::vector<core::BrokerCheckRow> checks = engine_.compare_with_broker(account);
        for (const core::BrokerCheckRow& c : checks) {
            ::optra::AccountPositionMsg* m = response->add_positions();
            m->set_symbol(c.symbol);
            m->set_quantity(c.broker);
            m->set_engine_quantity(c.engine);
            m->set_traded_here(c.traded_here);
            m->set_mismatch(c.is_mismatch());
            if (const auto it = valued.find(c.symbol); it != valued.end()) {
                const core::AccountPosition& p = *it->second;
                m->set_market_price(p.market_price);
                m->set_market_value(p.market_value);
                m->set_average_cost(p.average_cost);
                m->set_unrealized_pnl(p.unrealized_pnl);
                m->set_realized_pnl(p.realized_pnl);
            }
        }
        const auto [check, problem] =
            engine_.read([](const core::Main& m) { return m.bookkeeping_engine()->broker_check(); });
        response->set_broker_check(check);
        response->set_broker_check_problem(problem);
        for (const auto& [strategy, options] : engine_.read(
                 [](const core::Main& m) { return m.bookkeeping_engine()->unsettled_options(); })) {
            ::optra::UnsettledMsg* m = response->add_unsettled();
            m->set_strategy_name(strategy);
            for (const std::string& o : options) {
                m->add_options(o);
            }
        }
    });
}

auto GrpcLiveEngineService::CheckWithBroker(::grpc::ServerContext* /*context*/,
                                            const ::optra::Empty* /*request*/,
                                            ::optra::BrokerCheckResponse* response)
    -> ::grpc::Status {
    return status_of([&] {
        bool agree = true;
        for (const core::BrokerCheckRow& c : engine_.check_with_broker()) {
            if (c.is_mismatch()) {
                agree = false;
                diff_msg(c, response->add_mismatches());
            } else if (!c.traded_here && c.broker != 0.0) {
                diff_msg(c, response->add_external());
            }
        }
        response->set_agree(agree);
    });
}

auto GrpcLiveEngineService::SettleExpired(::grpc::ServerContext* /*context*/,
                                          const ::optra::SettleExpiredRequest* request,
                                          ::optra::SettleExpiredResponse* response)
    -> ::grpc::Status {
    return status_of([&] {
        response->set_settlements(engine_.settle_expired(
            request->portfolio(),
            model::parse_date(request->expiry()).value_or(std::chrono::year_month_day{}),
            request->price()));
    });
}

auto GrpcLiveEngineService::BookManually(::grpc::ServerContext* /*context*/,
                                         const ::optra::BookManuallyRequest* request,
                                         ::optra::BookManuallyResponse* response)
    -> ::grpc::Status {
    return status_of([&] {
        const std::string& side = request->side();
        if (side != "BUY" && side != "SELL") {
            throw std::invalid_argument("side is BUY or SELL");
        }
        response->set_order_id(engine_.book_manually(
            request->strategy(), request->symbol(),
            side == "BUY" ? model::Side::BUY : model::Side::SELL, request->quantity(),
            request->price(), request->reason()));
    });
}

void GrpcLiveEngineService::fill_risk(::optra::RiskState* response) {
    engine_.read([&](const core::Main& main) {
        const core::RiskEngine& risk = *main.risk_engine();
        response->set_halted(risk.halted());
        response->set_halt_reason(risk.halt_reason());
        if (risk.halted()) {
            response->set_halted_at(std::format(
                "{:%Y-%m-%d %H:%M:%S}",
                std::chrono::floor<std::chrono::seconds>(risk.halted_at())));
        }
        for (const model::Parameter& p : core::RiskEngine::parameters()) {
            ::optra::RiskLimit* limit = response->add_limits();
            limit->set_name(std::string(p.name));
            limit->set_value(risk.limit(p.name));
            limit->set_default_value(p.default_value);
            limit->set_min(p.min);
            limit->set_max(p.max);
            limit->set_integer(p.integer);
        }
        response->set_refused(risk.refused());
        for (const std::string& line : risk.recent_refusals()) {
            response->add_recent_refusals(line);
        }
        response->mutable_day_stops()->insert(risk.day_stops().begin(), risk.day_stops().end());
    });
}

auto GrpcLiveEngineService::GetRisk(::grpc::ServerContext* /*context*/,
                                    const ::optra::Empty* /*request*/,
                                    ::optra::RiskState* response) -> ::grpc::Status {
    return status_of([&] { fill_risk(response); });
}

auto GrpcLiveEngineService::SetRiskLimits(::grpc::ServerContext* /*context*/,
                                          const ::optra::SetRiskLimitsRequest* request,
                                          ::optra::RiskState* response) -> ::grpc::Status {
    return status_of([&] {
        engine_.set_risk_settings({request->values().begin(), request->values().end()});
        fill_risk(response);
    });
}

auto GrpcLiveEngineService::Halt(::grpc::ServerContext* /*context*/,
                                 const ::optra::HaltRequest* request,
                                 ::optra::RiskState* response) -> ::grpc::Status {
    return status_of([&] {
        engine_.halt(request->reason());
        fill_risk(response);
    });
}

auto GrpcLiveEngineService::ClearHalt(::grpc::ServerContext* /*context*/,
                                      const ::optra::Empty* /*request*/,
                                      ::optra::RiskState* response) -> ::grpc::Status {
    return status_of([&] {
        engine_.clear_halt();
        fill_risk(response);
    });
}

auto serve(const std::function<std::unique_ptr<core::Main>()>& make,
           const std::string& listen_address, std::vector<live::Synced> portfolios,
           bool resume, std::optional<std::chrono::seconds> lasts) -> int {
    sigset_t stop_signals;
    sigemptyset(&stop_signals);
    sigaddset(&stop_signals, SIGINT);
    sigaddset(&stop_signals, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &stop_signals, nullptr);

    // Static: the stopper outlives this function when it returns early (detached).
    static std::mutex serving_mutex;
    static grpc::Server* serving = nullptr;
    static live::TradingDays* rolling = nullptr; // a day it may be beginning
    std::thread stopper([stop_signals, lasts] {
        int signal = 0;
        if (lasts) {
            const timespec timeout{.tv_sec = static_cast<time_t>(lasts->count()), .tv_nsec = 0};
            signal = sigtimedwait(&stop_signals, nullptr, &timeout); // -1: the time is up
        } else {
            sigwait(&stop_signals, &signal);
        }
        const std::lock_guard lock(serving_mutex);
        if (serving == nullptr) {
            std::println("Stopping before the engine serves");
            std::fflush(stdout);
            std::_Exit(signal > 0 ? 128 + signal : 0);
        }
        if (signal > 0) {
            std::println("Signal {}: stopping", signal);
        } else {
            std::println("The time is up: stopping");
        }
        std::fflush(stdout);
        rolling->interrupt(); // a contract sync under way (IB slow) does not hold the stop up
        // A deadline: the streams the UI keeps open (logs, strategy updates) end only once
        // cancelled, and without one Shutdown waits for them for ever.
        serving->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds(2));
    });

    std::unique_ptr<core::Main> engine;
    try {
        engine = make();
    } catch (const std::exception& e) {
        std::println(stderr, "The engine did not start: {}", e.what());
        stopper.detach(); // it waits for a signal that will not come: the process ends
        return 1;
    }
    live::TradingDays days(*engine); // after the engine: stopped before it closes
    GrpcLiveEngineService service(*engine);
    grpc::ServerBuilder builder;
    builder.AddListeningPort(listen_address, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);
    std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
    if (!server) {
        std::println(stderr, "Failed to start gRPC server on {}", listen_address);
        stopper.detach();
        return 1;
    }
    {
        const std::lock_guard lock(serving_mutex);
        serving = server.get();
        rolling = &days;
    }
    std::println("Engine listening on {}", listen_address);
    std::fflush(stdout);

    days.start(std::move(portfolios), resume); // served meanwhile: the UI sees it come up
    server->Wait();
    stopper.join();
    days.stop();
    engine->close();
    return 0;
}

} // namespace entry
