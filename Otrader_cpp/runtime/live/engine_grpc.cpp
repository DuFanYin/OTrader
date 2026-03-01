#include "engine_grpc.hpp"
#include "../../core/engine_log.hpp"
#include "../../strategy/strategy_registry.hpp"
#include "../../strategy/template.hpp"
#include "../../utilities/event.hpp"
#include "engine_db_pg.hpp"

#include <cstdlib>
#include <exception>
#include <sstream>

namespace engines {

namespace {

auto parse_setting_json(const std::string& s) -> std::unordered_map<std::string, double> {
    std::unordered_map<std::string, double> out;
    if (s.empty() || s == "{}") {
        return out;
    }
    for (size_t i = 0; i < s.size();) {
        if (s[i] == '"') {
            ++i;
            size_t start = i;
            while (i < s.size() && s[i] != '"') {
                ++i;
            }
            std::string key = s.substr(start, i - start);
            if (i < s.size()) {
                ++i;
            }
            while (i < s.size() && (s[i] == ' ' || s[i] == ':')) {
                ++i;
            }
            double val = 0;
            if (i < s.size() && ((std::isdigit(static_cast<unsigned char>(s[i])) != 0) ||
                                 s[i] == '-' || s[i] == '.')) {
                char* end = nullptr;
                val = std::strtod(s.c_str() + i, &end);
                if (end != nullptr) {
                    i = static_cast<size_t>(end - s.c_str());
                }
            }
            out[key] = val;
            while (i < s.size() && s[i] != '"' && s[i] != '}') {
                ++i;
            }
        } else {
            ++i;
        }
    }
    return out;
}

} // namespace

GrpcLiveEngineService::GrpcLiveEngineService(MainEngine* main_engine) : main_engine_(main_engine) {}

auto GrpcLiveEngineService::GetStatus(::grpc::ServerContext* /*context*/,
                                      const ::otrader::Empty* /*request*/,
                                      ::otrader::EngineStatus* response) -> ::grpc::Status {
    if (response == nullptr) {
        return ::grpc::Status(::grpc::StatusCode::INTERNAL, "response is null");
    }

    const bool running = (main_engine_ != nullptr);
    response->set_running(running);

    const bool ib_connected = running && (main_engine_->ib_gateway() != nullptr) &&
                              main_engine_->ib_gateway()->is_connected();
    response->set_connected(ib_connected);

    const bool md_running = running && main_engine_->market_data_running();

    if (!running) {
        response->set_detail("engine: stopped; ib: off; md: off");
    } else {
        std::ostringstream os;
        os << "engine: running; ib: " << (ib_connected ? "on" : "off")
           << "; md: " << (md_running ? "on" : "off");
        response->set_detail(os.str());
    }
    return ::grpc::Status::OK;
}

auto GrpcLiveEngineService::ListStrategies(::grpc::ServerContext* /*context*/,
                                           const ::otrader::Empty* /*request*/,
                                           ::grpc::ServerWriter<::otrader::StrategySummary>* writer)
    -> ::grpc::Status {
    if ((writer == nullptr) || (main_engine_ == nullptr) ||
        (main_engine_->option_strategy_engine() == nullptr)) {
        return ::grpc::Status::OK;
    }
    auto* se = main_engine_->option_strategy_engine();
    for (const std::string& name : se->get_strategy_names()) {
        auto* s = se->get_strategy(name);
        if (s == nullptr) {
            continue;
        }
        ::otrader::StrategySummary sum;
        sum.set_strategy_name(s->strategy_name());
        sum.set_class_name(name.substr(0, name.find('_')));
        sum.set_portfolio(s->portfolio_name());
        if (s->error()) {
            sum.set_status("error");
        } else if (s->started()) {
            sum.set_status("running");
        } else if (s->inited()) {
            sum.set_status("stopped");
        } else {
            sum.set_status("created");
        }
        writer->Write(sum);
    }
    return ::grpc::Status::OK;
}

auto GrpcLiveEngineService::ConnectGateway(::grpc::ServerContext* /*context*/,
                                           const ::otrader::Empty* /*request*/,
                                           ::otrader::Empty* /*response*/) -> ::grpc::Status {
    if (main_engine_ == nullptr) {
        return ::grpc::Status(::grpc::StatusCode::FAILED_PRECONDITION, "main engine is null");
    }
    try {
        main_engine_->connect();
        return ::grpc::Status::OK;
    } catch (const std::exception& e) {
        return ::grpc::Status(::grpc::StatusCode::INTERNAL, e.what());
    }
}

auto GrpcLiveEngineService::DisconnectGateway(::grpc::ServerContext* /*context*/,
                                              const ::otrader::Empty* /*request*/,
                                              ::otrader::Empty* /*response*/) -> ::grpc::Status {
    if (main_engine_ == nullptr) {
        return ::grpc::Status(::grpc::StatusCode::FAILED_PRECONDITION, "main engine is null");
    }
    try {
        main_engine_->disconnect();
        return ::grpc::Status::OK;
    } catch (const std::exception& e) {
        return ::grpc::Status(::grpc::StatusCode::INTERNAL, e.what());
    }
}

auto GrpcLiveEngineService::StartMarketData(::grpc::ServerContext* /*context*/,
                                            const ::otrader::Empty* /*request*/,
                                            ::otrader::Empty* /*response*/) -> ::grpc::Status {
    if (main_engine_ == nullptr) {
        return ::grpc::Status(::grpc::StatusCode::FAILED_PRECONDITION, "main engine is null");
    }
    try {
        main_engine_->start_market_data_update();
        return ::grpc::Status::OK;
    } catch (const std::exception& e) {
        return ::grpc::Status(::grpc::StatusCode::INTERNAL, e.what());
    }
}

auto GrpcLiveEngineService::StopMarketData(::grpc::ServerContext* /*context*/,
                                           const ::otrader::Empty* /*request*/,
                                           ::otrader::Empty* /*response*/) -> ::grpc::Status {
    if (main_engine_ == nullptr) {
        return ::grpc::Status(::grpc::StatusCode::FAILED_PRECONDITION, "main engine is null");
    }
    try {
        main_engine_->stop_market_data_update();
        return ::grpc::Status::OK;
    } catch (const std::exception& e) {
        return ::grpc::Status(::grpc::StatusCode::INTERNAL, e.what());
    }
}

auto GrpcLiveEngineService::StartStrategy(::grpc::ServerContext* /*context*/,
                                          const ::otrader::StrategyNameRequest* request,
                                          ::otrader::Empty* /*response*/) -> ::grpc::Status {
    if ((main_engine_ == nullptr) || (main_engine_->option_strategy_engine() == nullptr) ||
        (request == nullptr)) {
        return ::grpc::Status(::grpc::StatusCode::INVALID_ARGUMENT, "missing request or engine");
    }
    try {
        main_engine_->option_strategy_engine()->start_strategy(request->strategy_name());
        return ::grpc::Status::OK;
    } catch (const std::exception& e) {
        return ::grpc::Status(::grpc::StatusCode::INVALID_ARGUMENT, e.what());
    }
}

auto GrpcLiveEngineService::StopStrategy(::grpc::ServerContext* /*context*/,
                                         const ::otrader::StrategyNameRequest* request,
                                         ::otrader::Empty* /*response*/) -> ::grpc::Status {
    if ((main_engine_ == nullptr) || (main_engine_->option_strategy_engine() == nullptr) ||
        (request == nullptr)) {
        return ::grpc::Status(::grpc::StatusCode::INVALID_ARGUMENT, "missing request or engine");
    }
    try {
        main_engine_->option_strategy_engine()->stop_strategy(request->strategy_name());
        return ::grpc::Status::OK;
    } catch (const std::exception& e) {
        return ::grpc::Status(::grpc::StatusCode::INVALID_ARGUMENT, e.what());
    }
}

auto GrpcLiveEngineService::StreamLogs(::grpc::ServerContext* context,
                                       const ::otrader::Empty* /*request*/,
                                       ::grpc::ServerWriter<::otrader::LogLine>* writer)
    -> ::grpc::Status {
    if ((main_engine_ == nullptr) || (writer == nullptr)) {
        return ::grpc::Status(::grpc::StatusCode::FAILED_PRECONDITION, "main engine is null");
    }
    while (!context->IsCancelled()) {
        utilities::LogData log;
        if (!main_engine_->pop_log_for_stream(log, 1000)) {
            continue; // timeout, loop again and check cancellation
        }
        ::otrader::LogLine msg;
        // Send structured JSON string; frontend is responsible for formatting.
        // Minimal escaping for quotes and backslashes in msg.
        std::string level_str = engines::level_to_string(log.level);
        auto escape = [](const std::string& in) -> std::string {
            std::string out;
            out.reserve(in.size());
            for (char c : in) {
                if (c == '\\' || c == '"') {
                    out.push_back('\\');
                }
                out.push_back(c);
            }
            return out;
        };
        std::string json = "{";
        json += R"("src":"live")";
        json += R"(,"time":")" + escape(log.time) + "\"";
        json += ",\"level\":" + std::to_string(log.level);
        json += R"(,"level_str":")" + escape(level_str) + "\"";
        json += R"(,"gateway":")" + escape(log.gateway_name) + "\"";
        json += R"(,"msg":")" + escape(log.msg) + "\"";
        json += "}";
        msg.set_line(json);
        if (!writer->Write(msg)) {
            break;
        }
    }
    return ::grpc::Status::OK;
}

auto GrpcLiveEngineService::StreamStrategyUpdates(
    ::grpc::ServerContext* context, const ::otrader::Empty* /*request*/,
    ::grpc::ServerWriter<::otrader::StrategyUpdate>* writer) -> ::grpc::Status {
    if ((main_engine_ == nullptr) || (writer == nullptr)) {
        return ::grpc::Status(::grpc::StatusCode::FAILED_PRECONDITION, "main engine is null");
    }
    while (!context->IsCancelled()) {
        utilities::StrategyUpdateData upd;
        if (!main_engine_->pop_strategy_update(upd, 1000)) {
            continue; // timeout, loop again and check cancellation
        }
        ::otrader::StrategyUpdate msg;
        msg.set_strategy_name(upd.strategy_name);
        msg.set_class_name(upd.class_name);
        msg.set_portfolio(upd.portfolio);
        msg.set_json_payload(upd.json_payload);
        if (!writer->Write(msg)) {
            break;
        }
    }
    return ::grpc::Status::OK;
}

auto GrpcLiveEngineService::GetOrdersAndTrades(::grpc::ServerContext* /*context*/,
                                               const ::otrader::Empty* /*request*/,
                                               ::otrader::OrdersAndTradesResponse* response)
    -> ::grpc::Status {
    if ((response == nullptr) || (main_engine_ == nullptr) ||
        (main_engine_->db_engine() == nullptr)) {
        return ::grpc::Status(::grpc::StatusCode::INTERNAL, "missing engine or response");
    }
    try {
        auto orders = main_engine_->db_engine()->get_all_history_orders();
        auto trades = main_engine_->db_engine()->get_all_history_trades();
        for (const auto& row : orders) {
            auto* r = response->add_orders();
            if (!row.empty()) {
                r->set_timestamp(row[0]);
            }
            if (row.size() > 1) {
                r->set_strategy_name(row[1]);
            }
            if (row.size() > 2) {
                r->set_orderid(row[2]);
            }
            if (row.size() > 3) {
                r->set_symbol(row[3]);
            }
            if (row.size() > 4) {
                r->set_exchange(row[4]);
            }
            if (row.size() > 5) {
                r->set_trading_class(row[5]);
            }
            if (row.size() > 6) {
                r->set_type(row[6]);
            }
            if (row.size() > 7) {
                r->set_direction(row[7]);
            }
            if (row.size() > 8 && !row[8].empty()) {
                r->set_price(std::stod(row[8]));
            }
            if (row.size() > 9 && !row[9].empty()) {
                r->set_volume(std::stod(row[9]));
            }
            if (row.size() > 10 && !row[10].empty()) {
                r->set_traded(std::stod(row[10]));
            }
            if (row.size() > 11) {
                r->set_status(row[11]);
            }
            if (row.size() > 12) {
                r->set_datetime(row[12]);
            }
            if (row.size() > 13) {
                r->set_reference(row[13]);
            }
            if (row.size() > 14 && !row[14].empty()) {
                r->set_is_combo(std::stoi(row[14]) != 0);
            }
            if (row.size() > 15) {
                r->set_legs_info(row[15]);
            }
        }
        for (const auto& row : trades) {
            auto* r = response->add_trades();
            if (!row.empty()) {
                r->set_timestamp(row[0]);
            }
            if (row.size() > 1) {
                r->set_strategy_name(row[1]);
            }
            if (row.size() > 2) {
                r->set_tradeid(row[2]);
            }
            if (row.size() > 3) {
                r->set_symbol(row[3]);
            }
            if (row.size() > 4) {
                r->set_exchange(row[4]);
            }
            if (row.size() > 5) {
                r->set_orderid(row[5]);
            }
            if (row.size() > 6) {
                r->set_direction(row[6]);
            }
            if (row.size() > 7 && !row[7].empty()) {
                r->set_price(std::stod(row[7]));
            }
            if (row.size() > 8 && !row[8].empty()) {
                r->set_volume(std::stod(row[8]));
            }
            if (row.size() > 9) {
                r->set_datetime(row[9]);
            }
        }
        return ::grpc::Status::OK;
    } catch (const std::exception& e) {
        return ::grpc::Status(::grpc::StatusCode::INTERNAL, e.what());
    }
}

auto GrpcLiveEngineService::ListPortfolios(::grpc::ServerContext* /*context*/,
                                           const ::otrader::Empty* /*request*/,
                                           ::otrader::ListPortfoliosResponse* response)
    -> ::grpc::Status {
    if ((response == nullptr) || (main_engine_ == nullptr)) {
        return ::grpc::Status::OK;
    }
    try {
        for (const std::string& n : main_engine_->get_all_portfolio_names()) {
            response->add_portfolios(n);
        }
        return ::grpc::Status::OK;
    } catch (const std::exception& e) {
        return ::grpc::Status(::grpc::StatusCode::INTERNAL, e.what());
    }
}

auto GrpcLiveEngineService::ListStrategyClasses(::grpc::ServerContext* /*context*/,
                                                const ::otrader::Empty* /*request*/,
                                                ::otrader::ListStrategyClassesResponse* response)
    -> ::grpc::Status {
    if (response == nullptr) {
        return ::grpc::Status::OK;
    }
    try {
        for (const std::string& c :
             strategy_cpp::StrategyRegistry::get_all_strategy_class_names()) {
            response->add_classes(c);
        }
        return ::grpc::Status::OK;
    } catch (const std::exception& e) {
        return ::grpc::Status(::grpc::StatusCode::INTERNAL, e.what());
    }
}

auto GrpcLiveEngineService::GetPortfoliosMeta(::grpc::ServerContext* /*context*/,
                                              const ::otrader::Empty* /*request*/,
                                              ::otrader::ListPortfoliosResponse* response)
    -> ::grpc::Status {
    return ListPortfolios(nullptr, nullptr, response);
}

auto GrpcLiveEngineService::GetRemovedStrategies(::grpc::ServerContext* /*context*/,
                                                 const ::otrader::Empty* /*request*/,
                                                 ::otrader::GetRemovedStrategiesResponse* response)
    -> ::grpc::Status {
    if (response == nullptr) {
        return ::grpc::Status::OK;
    }
    return ::grpc::Status::OK;
}

auto GrpcLiveEngineService::AddStrategy(::grpc::ServerContext* /*context*/,
                                        const ::otrader::AddStrategyRequest* request,
                                        ::otrader::AddStrategyResponse* response)
    -> ::grpc::Status {
    if ((request == nullptr) || (response == nullptr) || (main_engine_ == nullptr) ||
        (main_engine_->option_strategy_engine() == nullptr)) {
        return ::grpc::Status(::grpc::StatusCode::INVALID_ARGUMENT, "missing request or engine");
    }
    try {
        auto setting = parse_setting_json(request->setting_json());
        main_engine_->option_strategy_engine()->add_strategy(request->strategy_class(),
                                                             request->portfolio_name(), setting);
        response->set_strategy_name(request->strategy_class() + "_" + request->portfolio_name());
        return ::grpc::Status::OK;
    } catch (const std::exception& e) {
        return ::grpc::Status(::grpc::StatusCode::INVALID_ARGUMENT, e.what());
    }
}

auto GrpcLiveEngineService::RestoreStrategy(::grpc::ServerContext* /*context*/,
                                            const ::otrader::StrategyNameRequest* /*request*/,
                                            ::otrader::Empty* /*response*/) -> ::grpc::Status {
    return ::grpc::Status(::grpc::StatusCode::UNIMPLEMENTED,
                          "RestoreStrategy not supported in C++ live");
}

auto GrpcLiveEngineService::InitStrategy(::grpc::ServerContext* /*context*/,
                                         const ::otrader::StrategyNameRequest* request,
                                         ::otrader::Empty* /*response*/) -> ::grpc::Status {
    if ((main_engine_ == nullptr) || (main_engine_->option_strategy_engine() == nullptr) ||
        (request == nullptr)) {
        return ::grpc::Status(::grpc::StatusCode::INVALID_ARGUMENT, "missing request or engine");
    }
    try {
        main_engine_->option_strategy_engine()->init_strategy(request->strategy_name());
        return ::grpc::Status::OK;
    } catch (const std::exception& e) {
        return ::grpc::Status(::grpc::StatusCode::INVALID_ARGUMENT, e.what());
    }
}

auto GrpcLiveEngineService::RemoveStrategy(::grpc::ServerContext* /*context*/,
                                           const ::otrader::StrategyNameRequest* request,
                                           ::otrader::RemoveStrategyResponse* response)
    -> ::grpc::Status {
    if ((request == nullptr) || (response == nullptr) || (main_engine_ == nullptr) ||
        (main_engine_->option_strategy_engine() == nullptr)) {
        return ::grpc::Status(::grpc::StatusCode::INVALID_ARGUMENT, "missing request or engine");
    }
    response->set_removed(
        main_engine_->option_strategy_engine()->remove_strategy(request->strategy_name()));
    return ::grpc::Status::OK;
}

auto GrpcLiveEngineService::DeleteStrategy(::grpc::ServerContext* /*context*/,
                                           const ::otrader::StrategyNameRequest* request,
                                           ::otrader::DeleteStrategyResponse* response)
    -> ::grpc::Status {
    if ((request == nullptr) || (response == nullptr) || (main_engine_ == nullptr) ||
        (main_engine_->option_strategy_engine() == nullptr)) {
        return ::grpc::Status(::grpc::StatusCode::INVALID_ARGUMENT, "missing request or engine");
    }
    response->set_deleted(
        main_engine_->option_strategy_engine()->remove_strategy(request->strategy_name()));
    return ::grpc::Status::OK;
}

auto GrpcLiveEngineService::GetStrategyHoldings(::grpc::ServerContext* /*context*/,
                                                const ::otrader::Empty* /*request*/,
                                                ::otrader::StrategyHoldingsResponse* response)
    -> ::grpc::Status {
    if ((response == nullptr) || (main_engine_ == nullptr) ||
        (main_engine_->option_strategy_engine() == nullptr) ||
        (main_engine_->position_engine() == nullptr)) {
        return ::grpc::Status::OK;
    }
    try {
        for (const std::string& name :
             main_engine_->option_strategy_engine()->get_strategy_names()) {
            try {
                std::string json = main_engine_->position_engine()->serialize_holding(name);
                (*response->mutable_holdings())[name] = json;
            } catch (...) {
                continue;
            }
        }
        return ::grpc::Status::OK;
    } catch (const std::exception& e) {
        return ::grpc::Status(::grpc::StatusCode::INTERNAL, e.what());
    }
}

} // namespace engines
