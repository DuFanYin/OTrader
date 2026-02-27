#pragma once

#include "engine_main.hpp"

#include <grpcpp/grpcpp.h>

#include "../../proto/otrader_engine.grpc.pb.h"

namespace engines {

/**
 * GrpcLiveEngineService
 *
 * - 仅用于 live 场景，直接持有 Live MainEngine 指针。
 * - 不做额外抽象层，RPC 方法内部直接调用 MainEngine 接口。
 */
class GrpcLiveEngineService final : public ::otrader::EngineService::Service {
  public:
    explicit GrpcLiveEngineService(MainEngine* main_engine);

    // 基础状态 / 策略列表
    ::grpc::Status GetStatus(::grpc::ServerContext* context, const ::otrader::Empty* request,
                             ::otrader::EngineStatus* response) override;

    ::grpc::Status
    ListStrategies(::grpc::ServerContext* context, const ::otrader::Empty* request,
                   ::grpc::ServerWriter<::otrader::StrategySummary>* writer) override;

    // live 控制
    ::grpc::Status ConnectGateway(::grpc::ServerContext* context, const ::otrader::Empty* request,
                                  ::otrader::Empty* response) override;

    ::grpc::Status DisconnectGateway(::grpc::ServerContext* context,
                                     const ::otrader::Empty* request,
                                     ::otrader::Empty* response) override;

    ::grpc::Status StartMarketData(::grpc::ServerContext* context, const ::otrader::Empty* request,
                                   ::otrader::Empty* response) override;

    ::grpc::Status StopMarketData(::grpc::ServerContext* context, const ::otrader::Empty* request,
                                  ::otrader::Empty* response) override;

    // 策略控制（目前可以先返回 UNIMPLEMENTED，后续按需要补齐）
    ::grpc::Status StartStrategy(::grpc::ServerContext* context,
                                 const ::otrader::StrategyNameRequest* request,
                                 ::otrader::Empty* response) override;

    ::grpc::Status StopStrategy(::grpc::ServerContext* context,
                                const ::otrader::StrategyNameRequest* request,
                                ::otrader::Empty* response) override;

    // 事件流（暂时占位，后续可以接到现有日志 / 策略更新通道）
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

    ::grpc::Status QueryPortfolio(::grpc::ServerContext* context,
                                  const ::otrader::PortfolioRequest* request,
                                  ::otrader::Empty* response) override;

    ::grpc::Status ListStrategyClasses(::grpc::ServerContext* context,
                                       const ::otrader::Empty* request,
                                       ::otrader::ListStrategyClassesResponse* response) override;

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
    MainEngine* main_engine_; // 非拥有指针，由 entry_live_grpc 控制生命周期
};

} // namespace engines
