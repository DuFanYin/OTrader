#include "engine_grpc.hpp"
#include "../adapters/db/engine_db_sqlite.hpp"
#include "../model/event.hpp"
#include "../utilities/log_level.hpp"
#include "../model/utility.hpp"

#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <format>
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

/** setting_json as name → number; throws std::invalid_argument on anything else, so a bad
 * setting is reported instead of running on defaults. */
auto parse_setting_json(const std::string& s) -> std::unordered_map<std::string, double> {
    std::unordered_map<std::string, double> out;
    if (s.empty()) {
        return out;
    }
    const nlohmann::json j = nlohmann::json::parse(s, nullptr, /*allow_exceptions=*/false);
    if (!j.is_object()) {
        throw std::invalid_argument("setting_json is not a JSON object: " + s);
    }
    for (const auto& [key, v] : j.items()) {
        if (!v.is_number()) {
            throw std::invalid_argument("setting '" + key + "' is not a number");
        }
        out[key] = v.get<double>();
    }
    return out;
}

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

/** YYYY-MM-DD; an invalid date when it is not one. */
auto parse_date(const std::string& text) -> std::chrono::year_month_day {
    int y = 0;
    unsigned m = 0;
    unsigned d = 0;
    if (std::sscanf(text.c_str(), "%d-%u-%u", &y, &m, &d) != 3) {
        return {};
    }
    return {std::chrono::year{y}, std::chrono::month{m}, std::chrono::day{d}};
}

void diff_msg(const core::PositionCheck& c, ::optra::PositionDiffMsg* out) {
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

/** A strategy's status as the API shows it: its state as the database keeps it (added, inited,
 * running), or error. */
auto status_name(adapters::StrategyState state, bool error) -> std::string {
    return error ? "error" : adapters::to_string(state);
}

void position_msg(const model::BasePosition& pos, ::optra::BasePositionMsg* msg) {
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
    position_msg(holding.underlyingPosition, msg.mutable_underlying());
    for (const auto& [symbol, option] : holding.optionPositions) {
        ::optra::OptionPositionMsg& m = (*msg.mutable_options())[symbol];
        m.set_symbol(option.symbol);
        m.set_quantity(option.quantity);
        m.set_avg_cost(option.avg_cost);
        m.set_cost_value(option.cost_value);
        m.set_realized_pnl(option.realized_pnl);
        m.set_mid_price(option.mid_price);
        m.set_delta(option.delta);
        m.set_gamma(option.gamma);
        m.set_theta(option.theta);
        m.set_vega(option.vega);
        m.set_multiplier(option.multiplier);
        m.set_combo_type(model::to_string(option.combo_type.value_or(model::ComboType::SINGLE_LEG)));
        for (const model::OptionPositionData& leg : option.legs) {
            position_msg(leg, m.add_legs());
        }
    }
    const model::PortfolioSummary& s = holding.summary;
    ::optra::PortfolioSummaryMsg* sm = msg.mutable_summary();
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

GrpcLiveEngineService::GrpcLiveEngineService(core::live::MainEngine& engine) : engine_(engine) {}

auto GrpcLiveEngineService::GetStatus(::grpc::ServerContext* /*context*/,
                                      const ::optra::Empty* /*request*/,
                                      ::optra::EngineStatus* response) -> ::grpc::Status {
    const bool connected = engine_.connected();
    const bool md_running = engine_.market_data_running();
    const bool halted = engine_.on_event_thread([&] { return engine_.risk_engine()->halted(); });
    response->set_running(true);
    response->set_connected(connected);
    response->set_halted(halted);
    response->set_detail(std::format("engine: running; ib: {}; md: {}{}", connected ? "on" : "off",
                                     md_running ? "on" : "off", halted ? "; trading: halted" : ""));
    return ::grpc::Status::OK;
}

auto GrpcLiveEngineService::ListStrategies(::grpc::ServerContext* /*context*/,
                                           const ::optra::Empty* /*request*/,
                                           ::grpc::ServerWriter<::optra::StrategySummary>* writer)
    -> ::grpc::Status {
    return status_of([&] {
        std::set<std::string> in_error;
        for (const auto& [name, message] : engine_.get_strategy_errors()) {
            in_error.insert(name);
        }
        for (const adapters::StrategyRecord& r : engine_.strategies()) {
            ::optra::StrategySummary sum;
            sum.set_strategy_name(r.name);
            sum.set_class_name(r.class_name);
            sum.set_portfolio(r.portfolio);
            sum.set_status(status_name(r.state, in_error.contains(r.name)));
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
    return status_of([&] { engine_.disconnect(); });
}

auto GrpcLiveEngineService::StartMarketData(::grpc::ServerContext* /*context*/,
                                            const ::optra::Empty* /*request*/,
                                            ::optra::Empty* /*response*/) -> ::grpc::Status {
    return status_of([&] { engine_.start_market_data_update(); });
}

auto GrpcLiveEngineService::StopMarketData(::grpc::ServerContext* /*context*/,
                                           const ::optra::Empty* /*request*/,
                                           ::optra::Empty* /*response*/) -> ::grpc::Status {
    return status_of([&] { engine_.stop_market_data_update(); });
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
        utilities::LogData log;
        if (!engine_.pop_log_for_stream(log, 1000)) {
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

auto GrpcLiveEngineService::GetOrdersAndTrades(::grpc::ServerContext* /*context*/,
                                               const ::optra::OrdersAndTradesRequest* request,
                                               ::optra::OrdersAndTradesResponse* response)
    -> ::grpc::Status {
    return status_of([&] {
        if (request->limit() < 0) {
            throw std::invalid_argument("limit is negative");
        }
        const auto limit = static_cast<std::size_t>(request->limit());
        // As the tables keep them: the text the database writes.
        for (const adapters::SavedRecord& saved :
             engine_.saved_orders(request->strategy_name(), limit)) {
            const auto& o = std::get<model::OrderData>(saved.record);
            ::optra::OrderRecord* r = response->add_orders();
            r->set_timestamp(adapters::datetime_to_str(saved.saved_at));
            r->set_strategy_name(saved.strategy_name);
            r->set_orderid(o.orderid);
            r->set_symbol(o.symbol);
            r->set_trading_class(o.trading_class);
            r->set_type(model::to_string(o.type));
            r->set_direction(model::to_string(o.direction));
            r->set_price(o.price);
            r->set_volume(o.volume);
            r->set_traded(o.traded);
            r->set_status(model::to_string(o.status));
            r->set_datetime(o.datetime != model::DateTime{} ? adapters::datetime_to_str(o.datetime)
                                                            : "N/A");
            r->set_reference(o.reference);
            r->set_is_combo(o.is_combo);
        }
        for (const adapters::SavedRecord& saved :
             engine_.saved_trades(request->strategy_name(), limit)) {
            const auto& t = std::get<model::TradeData>(saved.record);
            ::optra::TradeRecord* r = response->add_trades();
            r->set_timestamp(adapters::datetime_to_str(saved.saved_at));
            r->set_strategy_name(saved.strategy_name);
            r->set_tradeid(t.tradeid);
            r->set_symbol(t.symbol);
            r->set_orderid(t.orderid);
            r->set_direction(model::to_string(t.direction));
            r->set_price(t.price);
            r->set_volume(t.volume);
            r->set_datetime(t.datetime != model::DateTime{} ? adapters::datetime_to_str(t.datetime)
                                                            : "");
        }
        for (const std::string& name : engine_.strategies_with_records()) {
            response->add_strategies(name);
        }
    });
}

auto GrpcLiveEngineService::ListSavedContracts(::grpc::ServerContext* /*context*/,
                                               const ::optra::Empty* /*request*/,
                                               ::optra::SavedContractsResponse* response)
    -> ::grpc::Status {
    return status_of([&] {
        for (const model::ContractData& c : engine_.saved_contracts()) {
            ::optra::ContractMsg* m = response->add_contracts();
            m->set_symbol(c.symbol);
            m->set_product(model::to_string(c.product));
            m->set_size(c.size);
            m->set_pricetick(c.pricetick);
            if (c.option_type) {
                m->set_option_type(model::to_string(*c.option_type));
            }
            m->set_option_strike(c.option_strike.value_or(0.0));
            if (c.option_expiry) {
                m->set_option_expiry(
                    std::format("{:%F}", model::new_york_date(*c.option_expiry)));
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
        for (const std::string& name : engine_.get_all_portfolio_names()) {
            response->add_portfolios(name);
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
        for (const model::ContractData& c :
             engine_.sync_contracts(request->portfolio(), request->max_dte())) {
            if (c.product == model::Product::OPTION && c.option_expiry) {
                ++options;
                expiries.insert(std::format("{:%F}", model::new_york_date(*c.option_expiry)));
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
        for (const std::string& c : core::live::MainEngine::strategy_classes()) {
            response->add_classes(c);
        }
    });
}

auto GrpcLiveEngineService::GetStrategyClassDefaults(
    ::grpc::ServerContext* /*context*/, const ::optra::GetStrategyClassDefaultsRequest* request,
    ::optra::GetStrategyClassDefaultsResponse* response) -> ::grpc::Status {
    return status_of([&] {
        const std::string& class_name = request->strategy_class();
        for (const auto& [key, value] : engine_.strategy_class_defaults(class_name)) {
            (*response->mutable_settings())[key] = value;
        }
    });
}

auto GrpcLiveEngineService::GetRemovedStrategies(::grpc::ServerContext* /*context*/,
                                                 const ::optra::Empty* /*request*/,
                                                 ::optra::GetRemovedStrategiesResponse* response)
    -> ::grpc::Status {
    return status_of([&] {
        for (const std::string& name : engine_.removed_strategies()) {
            response->add_removed_strategies(name);
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
            parse_setting_json(request->setting_json()), request->strategy_name()));
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
        for (const auto& [name, holding] : engine_.holdings()) {
            (*response->mutable_holdings())[name] = holding_msg(holding);
        }
    });
}

auto GrpcLiveEngineService::GetAccount(::grpc::ServerContext* /*context*/,
                                       const ::optra::Empty* /*request*/,
                                       ::optra::AccountResponse* response) -> ::grpc::Status {
    return status_of([&] {
        // IB's side when it can be asked; the engine's side always.
        adapters::BrokerAccount account;
        try {
            account = engine_.query_account();
            response->set_from_ib(true);
        } catch (const std::exception& e) {
            response->set_ib_error(e.what());
        }
        response->set_account(account.account);
        response->set_updated(account.updated);
        for (const adapters::AccountValue& v : account.values) {
            ::optra::AccountValueMsg* m = response->add_values();
            m->set_key(v.key);
            m->set_value(v.value);
            m->set_currency(v.currency);
        }
        // Each contract held here or at IB, with what IB says it is worth.
        std::map<std::string, const adapters::AccountPosition*> valued;
        for (const adapters::AccountPosition& p : account.positions) {
            valued[p.symbol] = &p;
        }
        // IB answered: this comparison is the check, so the rows and positions_check agree.
        const std::vector<core::PositionCheck> checks = response->from_ib()
                                                            ? engine_.reconcile_positions(account)
                                                            : engine_.check_positions(account);
        for (const core::PositionCheck& c : checks) {
            ::optra::AccountPositionMsg* m = response->add_positions();
            m->set_symbol(c.symbol);
            m->set_quantity(c.broker);
            m->set_engine_quantity(c.engine);
            m->set_traded_here(c.traded_here);
            m->set_mismatch(c.is_mismatch());
            if (const auto it = valued.find(c.symbol); it != valued.end()) {
                const adapters::AccountPosition& p = *it->second;
                m->set_market_price(p.market_price);
                m->set_market_value(p.market_value);
                m->set_average_cost(p.average_cost);
                m->set_unrealized_pnl(p.unrealized_pnl);
                m->set_realized_pnl(p.realized_pnl);
            }
        }
        const auto [check, problem] = engine_.positions_check();
        response->set_positions_check(check);
        response->set_positions_problem(problem);
        for (const auto& [strategy, options] : engine_.unsettled()) {
            ::optra::UnsettledMsg* m = response->add_unsettled();
            m->set_strategy_name(strategy);
            for (const std::string& o : options) {
                m->add_options(o);
            }
        }
    });
}

auto GrpcLiveEngineService::ReconcilePositions(::grpc::ServerContext* /*context*/,
                                               const ::optra::Empty* /*request*/,
                                               ::optra::ReconcileResponse* response)
    -> ::grpc::Status {
    return status_of([&] {
        bool agree = true;
        for (const core::PositionCheck& c : engine_.reconcile_positions()) {
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
            request->portfolio(), parse_date(request->expiry()), request->price()));
    });
}

auto GrpcLiveEngineService::BookManually(::grpc::ServerContext* /*context*/,
                                         const ::optra::BookManuallyRequest* request,
                                         ::optra::BookManuallyResponse* response)
    -> ::grpc::Status {
    return status_of([&] {
        const std::string& side = request->direction();
        if (side != "LONG" && side != "SHORT") {
            throw std::invalid_argument("direction is LONG or SHORT");
        }
        response->set_orderid(engine_.book_manually(
            request->strategy(), request->symbol(),
            side == "LONG" ? model::Direction::LONG : model::Direction::SHORT, request->volume(),
            request->price(), request->reason()));
    });
}

void GrpcLiveEngineService::fill_risk(::optra::RiskState* response) {
    engine_.on_event_thread([&] {
        const core::RiskEngine& risk = *engine_.risk_engine();
        response->set_halted(risk.halted());
        response->set_halt_reason(risk.halt_reason());
        if (risk.halted()) {
            response->set_halted_at(std::format(
                "{:%Y-%m-%d %H:%M:%S}",
                std::chrono::floor<std::chrono::seconds>(risk.halted_at())));
        }
        for (const strategy::StrategyParam& p : core::RiskEngine::parameters()) {
            ::optra::RiskLimit* limit = response->add_limits();
            limit->set_name(std::string(p.name));
            limit->set_value(risk.setting(p.name));
            limit->set_default_value(p.default_value);
            limit->set_min(p.min);
            limit->set_max(p.max);
            limit->set_integer(p.integer);
        }
        response->set_refused(risk.refused());
        for (const std::string& line : risk.recent_refusals()) {
            response->add_recent_refusals(line);
        }
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

} // namespace entry
