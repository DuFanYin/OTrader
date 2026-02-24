#include "engine_grpc.hpp"
#include "engine_db_pg.hpp"
#include "../../strategy/strategy_registry.hpp"
#include "../../strategy/template.hpp"
#include "../../utilities/event.hpp"

#include <cstdlib>
#include <exception>
#include <sstream>

namespace engines {

namespace {

std::unordered_map<std::string, double> parse_setting_json(const std::string& s) {
    std::unordered_map<std::string, double> out;
    if (s.empty() || s == "{}") return out;
    for (size_t i = 0; i < s.size(); ) {
        if (s[i] == '"') {
            ++i;
            size_t start = i;
            while (i < s.size() && s[i] != '"') ++i;
            std::string key = s.substr(start, i - start);
            if (i < s.size()) ++i;
            while (i < s.size() && (s[i] == ' ' || s[i] == ':')) ++i;
            double val = 0;
            if (i < s.size() && (std::isdigit(static_cast<unsigned char>(s[i])) || s[i] == '-' || s[i] == '.')) {
                char* end = nullptr;
                val = std::strtod(s.c_str() + i, &end);
                if (end) i = static_cast<size_t>(end - s.c_str());
            }
            out[key] = val;
            while (i < s.size() && s[i] != '"' && s[i] != '}') ++i;
        } else {
            ++i;
        }
    }
    return out;
}

}  // namespace

GrpcLiveEngineService::GrpcLiveEngineService(MainEngine* main_engine)
    : main_engine_(main_engine) {}

::grpc::Status GrpcLiveEngineService::GetStatus(::grpc::ServerContext*,
                                                const ::otrader::Empty*,
                                                ::otrader::EngineStatus* response) {
    if (!response) {
        return ::grpc::Status(::grpc::StatusCode::INTERNAL, "response is null");
    }
    response->set_running(true);
    bool connected = (main_engine_ && main_engine_->ib_gateway() && main_engine_->ib_gateway()->is_connected());
    response->set_connected(connected);
    response->set_detail("live main engine");
    return ::grpc::Status::OK;
}

::grpc::Status GrpcLiveEngineService::ListStrategies(
    ::grpc::ServerContext*,
    const ::otrader::Empty*,
    ::grpc::ServerWriter<::otrader::StrategySummary>* writer) {
    if (!writer || !main_engine_ || !main_engine_->option_strategy_engine()) {
        return ::grpc::Status::OK;
    }
    auto* se = main_engine_->option_strategy_engine();
    for (const std::string& name : se->get_strategy_names()) {
        auto* s = se->get_strategy(name);
        if (!s) continue;
        ::otrader::StrategySummary sum;
        sum.set_strategy_name(s->strategy_name());
        sum.set_class_name(name.substr(0, name.find('_')));
        sum.set_portfolio(s->portfolio_name());
        if (s->error()) sum.set_status("error");
        else if (s->started()) sum.set_status("running");
        else if (s->inited()) sum.set_status("stopped");
        else sum.set_status("created");
        writer->Write(sum);
    }
    return ::grpc::Status::OK;
}

::grpc::Status GrpcLiveEngineService::ConnectGateway(::grpc::ServerContext*,
                                                     const ::otrader::Empty*,
                                                     ::otrader::Empty*) {
    if (!main_engine_) {
        return ::grpc::Status(::grpc::StatusCode::FAILED_PRECONDITION, "main engine is null");
    }
    try {
        main_engine_->connect();
        return ::grpc::Status::OK;
    } catch (const std::exception& e) {
        return ::grpc::Status(::grpc::StatusCode::INTERNAL, e.what());
    }
}

::grpc::Status GrpcLiveEngineService::DisconnectGateway(::grpc::ServerContext*,
                                                        const ::otrader::Empty*,
                                                        ::otrader::Empty*) {
    if (!main_engine_) {
        return ::grpc::Status(::grpc::StatusCode::FAILED_PRECONDITION, "main engine is null");
    }
    try {
        main_engine_->disconnect();
        return ::grpc::Status::OK;
    } catch (const std::exception& e) {
        return ::grpc::Status(::grpc::StatusCode::INTERNAL, e.what());
    }
}

::grpc::Status GrpcLiveEngineService::StartMarketData(::grpc::ServerContext*,
                                                      const ::otrader::Empty*,
                                                      ::otrader::Empty*) {
    if (!main_engine_) {
        return ::grpc::Status(::grpc::StatusCode::FAILED_PRECONDITION, "main engine is null");
    }
    try {
        main_engine_->start_market_data_update();
        return ::grpc::Status::OK;
    } catch (const std::exception& e) {
        return ::grpc::Status(::grpc::StatusCode::INTERNAL, e.what());
    }
}

::grpc::Status GrpcLiveEngineService::StopMarketData(::grpc::ServerContext*,
                                                     const ::otrader::Empty*,
                                                     ::otrader::Empty*) {
    if (!main_engine_) {
        return ::grpc::Status(::grpc::StatusCode::FAILED_PRECONDITION, "main engine is null");
    }
    try {
        main_engine_->stop_market_data_update();
        return ::grpc::Status::OK;
    } catch (const std::exception& e) {
        return ::grpc::Status(::grpc::StatusCode::INTERNAL, e.what());
    }
}

::grpc::Status GrpcLiveEngineService::StartStrategy(::grpc::ServerContext*,
                                                    const ::otrader::StrategyNameRequest* request,
                                                    ::otrader::Empty*) {
    if (!main_engine_ || !main_engine_->option_strategy_engine() || !request) {
        return ::grpc::Status(::grpc::StatusCode::INVALID_ARGUMENT, "missing request or engine");
    }
    try {
        main_engine_->option_strategy_engine()->start_strategy(request->strategy_name());
        return ::grpc::Status::OK;
    } catch (const std::exception& e) {
        return ::grpc::Status(::grpc::StatusCode::INVALID_ARGUMENT, e.what());
    }
}

::grpc::Status GrpcLiveEngineService::StopStrategy(::grpc::ServerContext*,
                                                   const ::otrader::StrategyNameRequest* request,
                                                   ::otrader::Empty*) {
    if (!main_engine_ || !main_engine_->option_strategy_engine() || !request) {
        return ::grpc::Status(::grpc::StatusCode::INVALID_ARGUMENT, "missing request or engine");
    }
    try {
        main_engine_->option_strategy_engine()->stop_strategy(request->strategy_name());
        return ::grpc::Status::OK;
    } catch (const std::exception& e) {
        return ::grpc::Status(::grpc::StatusCode::INVALID_ARGUMENT, e.what());
    }
}

::grpc::Status GrpcLiveEngineService::StreamLogs(::grpc::ServerContext*,
                                                 const ::otrader::Empty*,
                                                 ::grpc::ServerWriter<::otrader::LogLine>*) {
    // 目前日志流由 backend 自己通过 WebSocket 处理；gRPC 日志流可以后续再接。
    return ::grpc::Status(::grpc::StatusCode::UNIMPLEMENTED, "StreamLogs not implemented yet");
}

::grpc::Status GrpcLiveEngineService::StreamStrategyUpdates(
    ::grpc::ServerContext* context,
    const ::otrader::Empty*,
    ::grpc::ServerWriter<::otrader::StrategyUpdate>* writer) {
    if (!main_engine_ || !writer) {
        return ::grpc::Status(::grpc::StatusCode::FAILED_PRECONDITION, "main engine is null");
    }
    while (!context->IsCancelled()) {
        utilities::StrategyUpdateData upd;
        if (!main_engine_->pop_strategy_update(upd, 1000)) {
            continue;  // timeout, loop again and check cancellation
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

::grpc::Status GrpcLiveEngineService::GetOrdersAndTrades(::grpc::ServerContext*,
                                                         const ::otrader::Empty*,
                                                         ::otrader::OrdersAndTradesResponse* response) {
    if (!response || !main_engine_ || !main_engine_->db_engine()) {
        return ::grpc::Status(::grpc::StatusCode::INTERNAL, "missing engine or response");
    }
    try {
        auto orders = main_engine_->db_engine()->get_all_history_orders();
        auto trades = main_engine_->db_engine()->get_all_history_trades();
        for (const auto& row : orders) {
            auto* r = response->add_orders();
            if (row.size() > 0) r->set_timestamp(row[0]);
            if (row.size() > 1) r->set_strategy_name(row[1]);
            if (row.size() > 2) r->set_orderid(row[2]);
            if (row.size() > 3) r->set_symbol(row[3]);
            if (row.size() > 4) r->set_exchange(row[4]);
            if (row.size() > 5) r->set_trading_class(row[5]);
            if (row.size() > 6) r->set_type(row[6]);
            if (row.size() > 7) r->set_direction(row[7]);
            if (row.size() > 8 && !row[8].empty()) r->set_price(std::stod(row[8]));
            if (row.size() > 9 && !row[9].empty()) r->set_volume(std::stod(row[9]));
            if (row.size() > 10 && !row[10].empty()) r->set_traded(std::stod(row[10]));
            if (row.size() > 11) r->set_status(row[11]);
            if (row.size() > 12) r->set_datetime(row[12]);
            if (row.size() > 13) r->set_reference(row[13]);
            if (row.size() > 14 && !row[14].empty()) r->set_is_combo(std::stoi(row[14]) != 0);
            if (row.size() > 15) r->set_legs_info(row[15]);
        }
        for (const auto& row : trades) {
            auto* r = response->add_trades();
            if (row.size() > 0) r->set_timestamp(row[0]);
            if (row.size() > 1) r->set_strategy_name(row[1]);
            if (row.size() > 2) r->set_tradeid(row[2]);
            if (row.size() > 3) r->set_symbol(row[3]);
            if (row.size() > 4) r->set_exchange(row[4]);
            if (row.size() > 5) r->set_orderid(row[5]);
            if (row.size() > 6) r->set_direction(row[6]);
            if (row.size() > 7 && !row[7].empty()) r->set_price(std::stod(row[7]));
            if (row.size() > 8 && !row[8].empty()) r->set_volume(std::stod(row[8]));
            if (row.size() > 9) r->set_datetime(row[9]);
        }
        return ::grpc::Status::OK;
    } catch (const std::exception& e) {
        return ::grpc::Status(::grpc::StatusCode::INTERNAL, e.what());
    }
}

::grpc::Status GrpcLiveEngineService::ListPortfolios(::grpc::ServerContext*,
                                                    const ::otrader::Empty*,
                                                    ::otrader::ListPortfoliosResponse* response) {
    if (!response || !main_engine_) return ::grpc::Status::OK;
    try {
        for (const std::string& n : main_engine_->get_all_portfolio_names())
            response->add_portfolios(n);
        return ::grpc::Status::OK;
    } catch (const std::exception& e) {
        return ::grpc::Status(::grpc::StatusCode::INTERNAL, e.what());
    }
}

::grpc::Status GrpcLiveEngineService::ListStrategyClasses(::grpc::ServerContext*,
                                                          const ::otrader::Empty*,
                                                          ::otrader::ListStrategyClassesResponse* response) {
    if (!response) return ::grpc::Status::OK;
    try {
        for (const std::string& c : strategy_cpp::StrategyRegistry::get_all_strategy_class_names())
            response->add_classes(c);
        return ::grpc::Status::OK;
    } catch (const std::exception& e) {
        return ::grpc::Status(::grpc::StatusCode::INTERNAL, e.what());
    }
}

::grpc::Status GrpcLiveEngineService::GetPortfoliosMeta(::grpc::ServerContext*,
                                                       const ::otrader::Empty*,
                                                       ::otrader::ListPortfoliosResponse* response) {
    return ListPortfolios(nullptr, nullptr, response);
}

::grpc::Status GrpcLiveEngineService::GetRemovedStrategies(::grpc::ServerContext*,
                                                          const ::otrader::Empty*,
                                                          ::otrader::GetRemovedStrategiesResponse* response) {
    if (!response) return ::grpc::Status::OK;
    return ::grpc::Status::OK;
}

::grpc::Status GrpcLiveEngineService::AddStrategy(::grpc::ServerContext*,
                                                 const ::otrader::AddStrategyRequest* request,
                                                 ::otrader::AddStrategyResponse* response) {
    if (!request || !response || !main_engine_ || !main_engine_->option_strategy_engine()) {
        return ::grpc::Status(::grpc::StatusCode::INVALID_ARGUMENT, "missing request or engine");
    }
    try {
        auto setting = parse_setting_json(request->setting_json());
        main_engine_->option_strategy_engine()->add_strategy(
            request->strategy_class(), request->portfolio_name(), setting);
        response->set_strategy_name(request->strategy_class() + "_" + request->portfolio_name());
        return ::grpc::Status::OK;
    } catch (const std::exception& e) {
        return ::grpc::Status(::grpc::StatusCode::INVALID_ARGUMENT, e.what());
    }
}

::grpc::Status GrpcLiveEngineService::RestoreStrategy(::grpc::ServerContext*,
                                                      const ::otrader::StrategyNameRequest*,
                                                      ::otrader::Empty*) {
    return ::grpc::Status(::grpc::StatusCode::UNIMPLEMENTED, "RestoreStrategy not supported in C++ live");
}

::grpc::Status GrpcLiveEngineService::InitStrategy(::grpc::ServerContext*,
                                                   const ::otrader::StrategyNameRequest* request,
                                                   ::otrader::Empty*) {
    if (!main_engine_ || !main_engine_->option_strategy_engine() || !request) {
        return ::grpc::Status(::grpc::StatusCode::INVALID_ARGUMENT, "missing request or engine");
    }
    try {
        main_engine_->option_strategy_engine()->init_strategy(request->strategy_name());
        return ::grpc::Status::OK;
    } catch (const std::exception& e) {
        return ::grpc::Status(::grpc::StatusCode::INVALID_ARGUMENT, e.what());
    }
}

::grpc::Status GrpcLiveEngineService::RemoveStrategy(::grpc::ServerContext*,
                                                     const ::otrader::StrategyNameRequest* request,
                                                     ::otrader::RemoveStrategyResponse* response) {
    if (!request || !response || !main_engine_ || !main_engine_->option_strategy_engine()) {
        return ::grpc::Status(::grpc::StatusCode::INVALID_ARGUMENT, "missing request or engine");
    }
    response->set_removed(main_engine_->option_strategy_engine()->remove_strategy(request->strategy_name()));
    return ::grpc::Status::OK;
}

::grpc::Status GrpcLiveEngineService::DeleteStrategy(::grpc::ServerContext*,
                                                     const ::otrader::StrategyNameRequest* request,
                                                     ::otrader::DeleteStrategyResponse* response) {
    if (!request || !response || !main_engine_ || !main_engine_->option_strategy_engine()) {
        return ::grpc::Status(::grpc::StatusCode::INVALID_ARGUMENT, "missing request or engine");
    }
    response->set_deleted(main_engine_->option_strategy_engine()->remove_strategy(request->strategy_name()));
    return ::grpc::Status::OK;
}

::grpc::Status GrpcLiveEngineService::GetStrategyHoldings(::grpc::ServerContext*,
                                                         const ::otrader::Empty*,
                                                         ::otrader::StrategyHoldingsResponse* response) {
    if (!response || !main_engine_ || !main_engine_->option_strategy_engine() || !main_engine_->position_engine()) {
        return ::grpc::Status::OK;
    }
    try {
        for (const std::string& name : main_engine_->option_strategy_engine()->get_strategy_names()) {
            try {
                std::string json = main_engine_->position_engine()->serialize_holding(name);
                (*response->mutable_holdings())[name] = json;
            } catch (...) { continue; }
        }
        return ::grpc::Status::OK;
    } catch (const std::exception& e) {
        return ::grpc::Status(::grpc::StatusCode::INTERNAL, e.what());
    }
}

}  // namespace engines

