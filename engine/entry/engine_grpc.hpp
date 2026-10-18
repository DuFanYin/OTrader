#pragma once

#include "assembly/live.hpp"

#include <atomic>
#include <cstdint>
#include <vector>
#include <string>
#include <optional>
#include <memory>
#include <functional>
#include <chrono>
#include <grpcpp/grpcpp.h>

#include "optra_engine.grpc.pb.h"

namespace entry {

/**
 * The gRPC control plane over core::Main: each RPC translates between its plain data and the
 * messages. Each stream has one reader: a new subscription ends the one before (the backend fans
 * out).
 */
class GrpcLiveEngineService final : public ::optra::EngineService::Service {
  public:
    /** engine: owned by serve, outlives the service. */
    explicit GrpcLiveEngineService(core::Main& engine);

    // Status / strategy list
    ::grpc::Status GetStatus(::grpc::ServerContext* context, const ::optra::Empty* request,
                             ::optra::EngineStatus* response) override;

    ::grpc::Status
    ListStrategies(::grpc::ServerContext* context, const ::optra::Empty* request,
                   ::grpc::ServerWriter<::optra::StrategySummary>* writer) override;

    // Live control
    ::grpc::Status ConnectGateway(::grpc::ServerContext* context, const ::optra::Empty* request,
                                  ::optra::Empty* response) override;

    ::grpc::Status DisconnectGateway(::grpc::ServerContext* context,
                                     const ::optra::Empty* request,
                                     ::optra::Empty* response) override;

    ::grpc::Status StartMarketData(::grpc::ServerContext* context, const ::optra::Empty* request,
                                   ::optra::Empty* response) override;

    ::grpc::Status StopMarketData(::grpc::ServerContext* context, const ::optra::Empty* request,
                                  ::optra::Empty* response) override;

    // Strategy control
    ::grpc::Status StartStrategy(::grpc::ServerContext* context,
                                 const ::optra::StrategyNameRequest* request,
                                 ::optra::Empty* response) override;

    ::grpc::Status StopStrategy(::grpc::ServerContext* context,
                                const ::optra::StrategyNameRequest* request,
                                ::optra::Empty* response) override;

    // The log stream
    ::grpc::Status StreamLogs(::grpc::ServerContext* context, const ::optra::Empty* request,
                              ::grpc::ServerWriter<::optra::LogLine>* writer) override;

    // The database
    ::grpc::Status GetOrdersAndFills(::grpc::ServerContext* context,
                                     const ::optra::OrdersAndFillsRequest* request,
                                     ::optra::OrdersAndFillsResponse* response) override;

    ::grpc::Status ListSavedContracts(::grpc::ServerContext* context,
                                      const ::optra::Empty* request,
                                      ::optra::SavedContractsResponse* response) override;

    ::grpc::Status WipeDatabase(::grpc::ServerContext* context,
                                const ::optra::WipeDatabaseRequest* request,
                                ::optra::WipeDatabaseResponse* response) override;

    ::grpc::Status ListPortfolios(::grpc::ServerContext* context, const ::optra::Empty* request,
                                  ::optra::ListPortfoliosResponse* response) override;
    ::grpc::Status GetQuotes(::grpc::ServerContext* context, const ::optra::QuotesRequest* request,
                             ::optra::QuotesResponse* response) override;

    ::grpc::Status SyncContracts(::grpc::ServerContext* context,
                                 const ::optra::SyncContractsRequest* request,
                                 ::optra::SyncContractsResponse* response) override;

    ::grpc::Status ListStrategyClasses(::grpc::ServerContext* context,
                                       const ::optra::Empty* request,
                                       ::optra::ListStrategyClassesResponse* response) override;

    ::grpc::Status GetRemovedStrategies(::grpc::ServerContext* context,
                                        const ::optra::Empty* request,
                                        ::optra::GetRemovedStrategiesResponse* response) override;

    ::grpc::Status AddStrategy(::grpc::ServerContext* context,
                               const ::optra::AddStrategyRequest* request,
                               ::optra::AddStrategyResponse* response) override;

    ::grpc::Status RestoreStrategy(::grpc::ServerContext* context,
                                   const ::optra::StrategyNameRequest* request,
                                   ::optra::Empty* response) override;

    ::grpc::Status InitStrategy(::grpc::ServerContext* context,
                                const ::optra::StrategyNameRequest* request,
                                ::optra::Empty* response) override;

    ::grpc::Status RemoveStrategy(::grpc::ServerContext* context,
                                  const ::optra::StrategyNameRequest* request,
                                  ::optra::RemoveStrategyResponse* response) override;

    ::grpc::Status DeleteStrategy(::grpc::ServerContext* context,
                                  const ::optra::StrategyNameRequest* request,
                                  ::optra::DeleteStrategyResponse* response) override;

    ::grpc::Status GetStrategyHoldings(::grpc::ServerContext* context,
                                       const ::optra::Empty* request,
                                       ::optra::StrategyHoldingsResponse* response) override;

    ::grpc::Status GetAccount(::grpc::ServerContext* context, const ::optra::Empty* request,
                              ::optra::AccountResponse* response) override;
    ::grpc::Status CheckWithBroker(::grpc::ServerContext* context,
                                   const ::optra::Empty* request,
                                   ::optra::BrokerCheckResponse* response) override;
    ::grpc::Status SettleExpired(::grpc::ServerContext* context,
                                 const ::optra::SettleExpiredRequest* request,
                                 ::optra::SettleExpiredResponse* response) override;
    ::grpc::Status BookManually(::grpc::ServerContext* context,
                                const ::optra::BookManuallyRequest* request,
                                ::optra::BookManuallyResponse* response) override;
    ::grpc::Status GetRisk(::grpc::ServerContext* context, const ::optra::Empty* request,
                           ::optra::RiskState* response) override;
    ::grpc::Status SetRiskLimits(::grpc::ServerContext* context,
                                 const ::optra::SetRiskLimitsRequest* request,
                                 ::optra::RiskState* response) override;
    ::grpc::Status Halt(::grpc::ServerContext* context, const ::optra::HaltRequest* request,
                        ::optra::RiskState* response) override;
    ::grpc::Status ClearHalt(::grpc::ServerContext* context, const ::optra::Empty* request,
                             ::optra::RiskState* response) override;

  private:
    /** The risk limits, the halt and the refusals, as they stand (event thread). */
    void fill_risk(::optra::RiskState* response);

    core::Main& engine_;
    const std::string code_; // built_code(): what GetStatus reports
    // The latest subscription to each stream: an older one ends when it sees a newer.
    std::atomic<std::uint64_t> log_subscription_{0};
};

/**
 * Runs the engine as a process (entry_live, entry_simulate): SIGINT/SIGTERM blocked before the
 * engine makes threads and taken by a thread of their own; builds it (failure: exit 1), serves gRPC
 * at listen_address, starts TradingDays. On a signal or after `lasts`, shuts gRPC down, stops the
 * run and closes the engine. A signal before gRPC serves exits at once (nothing trades yet).
 * Returns the exit code.
 */
int serve(const std::function<std::unique_ptr<core::Main>()>& make,
          const std::string& listen_address, std::vector<live::Synced> portfolios,
          bool resume, std::optional<std::chrono::seconds> lasts = std::nullopt);

} // namespace entry
