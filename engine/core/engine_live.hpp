#pragma once

#include "adapters/engine_gateway.hpp"
#include "adapters/marketdata/engine_market_data.hpp"
#include "core/engine_main.hpp"
#include <functional>
#include <memory>
#include <string>

namespace core::live {

/** The live build's venue: IbGateway (set up from the environment) with IB (BUILD_GATEWAY), else
 * the base Gateway (no broker: orders are rejected). */
std::unique_ptr<adapters::Gateway> make_gateway(utilities::Logger* logger);

/**
 * The live assembly of core::Engine: a queue and one thread (EventEngine, started), the wall
 * clock, the broker (make_gateway), SQLite at $OPTRA_DB, and the market data (IB quotes on a
 * connection of their own). It starts where it stopped: the portfolios from the contracts saved, the strategies
 * as the operator left them (Engine::restore). Its public methods, with Engine's, are all the
 * control plane (GrpcLiveEngineService) reaches.
 */
class MainEngine : public core::Engine {
  public:
    using Clock = std::function<model::DateTime()>;
    using MakeGateway =
        std::function<std::unique_ptr<adapters::Gateway>(const model::PortfolioStructure&)>;

    /** gateway: the broker's; none, the build's (make_gateway). */
    explicit MainEngine(std::unique_ptr<adapters::Gateway> gateway = nullptr);
    /** Another venue and source of quotes than the build's, on their own clock (a simulated
     * market: entry_simulate). make_gateway gets the portfolios the quotes go to. */
    MainEngine(const MakeGateway& make_gateway, std::unique_ptr<adapters::MarketDataFeed> feed,
               Clock clock);
    ~MainEngine();

    adapters::MarketDataEngine* market_data() { return market_data_.get(); }

    void start_market_data_update();
    void stop_market_data_update();
    bool market_data_running() const { return market_data_ && market_data_->running(); }

    /** After a restart: connect, start the market data, and start the strategies that were
     * running. A step that fails is logged and the rest is left to the operator. */
    void resume();

  private:
    /** Everything but the clock, the venue and the source of quotes. */
    void assemble(std::unique_ptr<adapters::Gateway> gateway,
                  std::unique_ptr<adapters::MarketDataFeed> feed);
    void log_self_check();
};

} // namespace core::live
