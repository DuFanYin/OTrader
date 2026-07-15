#pragma once

#include "engine_main.hpp"

#include <grpcpp/grpcpp.h>

#include "../../proto/otrader_engine.grpc.pb.h"

namespace engines {

/**
 * GrpcLiveEngineService: live only, holds MainEngine*; RPC calls MainEngine directly.
 */
class GrpcLiveEngineService final : public ::otrader::EngineService::Service {
  public:
    explicit GrpcLiveEngineService(MainEngine* main_engine);

    // Status / strategy list
    ::grpc::Status GetStatus(::grpc::ServerContext* context, const ::otrader::Empty* request,
                             ::otrader::EngineStatus* response) override;

    ::grpc::Status
    ListStrategies(::grpc::ServerContext* context, const ::otrader::Empty* request,
                   ::grpc::ServerWriter<::otrader::StrategySummary>* writer) override;

    // Live control
    ::grpc::Status ConnectGateway(::grpc::ServerContext* context, const ::otrader::Empty* request,
                                  ::otrader::Empty* response) override;

    ::grpc::Status DisconnectGateway(::grpc::ServerContext* context,
                                     const ::otrader::Empty* request,
                                     ::otrader::Empty* response) override;

    ::grpc::Status StartMarketData(::grpc::ServerContext* context, const ::otrader::Empty* request,
                                   ::otrader::Empty* response) override;

    ::grpc::Status StopMarketData(::grpc::ServerContext* context, const ::otrader::Empty* request,
                                  ::otrader::Empty* response) override;

    // Strategy control
    ::grpc::Status StartStrategy(::grpc::ServerContext* context,
                                 const ::otrader::StrategyNameRequest* request,
                                 ::otrader::Empty* response) override;

    ::grpc::Status StopStrategy(::grpc::ServerContext* context,
                                const ::otrader::StrategyNameRequest* request,
                                ::otrader::Empty* response) override;

    // Event streams (logs / strategy updates)
    ::grpc::Status StreamLogs(::grpc::ServerContext* context, const ::otrader::Empty* request,
                              ::grpc::ServerWriter<::otrader::LogLine>* writer) override;

    ::grpc::Status
    StreamStrategyUpdates(::grpc::ServerContext* context, const ::otrader::Empty* request,
                          ::grpc::ServerWriter<::otrader::StrategyUpdate>* writer) override;

    ::grpc::Status GetOrdersAndTrades(::grpc::ServerContext* context,
                                      const ::otrader::Empty* request,
                                      ::otrader::OrdersAndTradesResponse* response) override;

    ::grpc::Status ListPortfolios(::grpc::ServerContext* context, const ::otrader::Empty* request,
                                  ::otrader::ListPortfoliosResponse* response) override;

    ::grpc::Status ListStrategyClasses(::grpc::ServerContext* context,
                                       const ::otrader::Empty* request,
                                       ::otrader::ListStrategyClassesResponse* response) override;

    ::grpc::Status
    GetStrategyClassDefaults(::grpc::ServerContext* context,
                             const ::otrader::GetStrategyClassDefaultsRequest* request,
                             ::otrader::GetStrategyClassDefaultsResponse* response) override;

    ::grpc::Status GetPortfoliosMeta(::grpc::ServerContext* context,
                                     const ::otrader::Empty* request,
                                     ::otrader::ListPortfoliosResponse* response) override;

    ::grpc::Status GetRemovedStrategies(::grpc::ServerContext* context,
                                        const ::otrader::Empty* request,
                                        ::otrader::GetRemovedStrategiesResponse* response) override;

    ::grpc::Status AddStrategy(::grpc::ServerContext* context,
                               const ::otrader::AddStrategyRequest* request,
                               ::otrader::AddStrategyResponse* response) override;

    ::grpc::Status RestoreStrategy(::grpc::ServerContext* context,
                                   const ::otrader::StrategyNameRequest* request,
                                   ::otrader::Empty* response) override;

    ::grpc::Status InitStrategy(::grpc::ServerContext* context,
                                const ::otrader::StrategyNameRequest* request,
                                ::otrader::Empty* response) override;

    ::grpc::Status RemoveStrategy(::grpc::ServerContext* context,
                                  const ::otrader::StrategyNameRequest* request,
                                  ::otrader::RemoveStrategyResponse* response) override;

    ::grpc::Status DeleteStrategy(::grpc::ServerContext* context,
                                  const ::otrader::StrategyNameRequest* request,
                                  ::otrader::DeleteStrategyResponse* response) override;

    ::grpc::Status GetStrategyHoldings(::grpc::ServerContext* context,
                                       const ::otrader::Empty* request,
                                       ::otrader::StrategyHoldingsResponse* response) override;

  private:
    MainEngine* main_engine_; // Non-owning; lifecycle by entry_live_grpc
};

} // namespace engines
