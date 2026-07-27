#include "engine_gateway_null.hpp"
#include <format>

namespace engines {

NullGateway::NullGateway(runtime_common::MainEngineBase* main_engine)
    : utilities::BaseEngine(main_engine, "NullGateway") {}

void NullGateway::connect() {
    write_log("null gateway: built without IB (BUILD_GATEWAY=OFF); orders will be rejected",
              WARNING);
}

void NullGateway::disconnect() {}

auto NullGateway::send_order(const utilities::OrderRequest& req) -> std::string {
    write_log(std::format("null gateway: rejecting order symbol={} vol={} (no broker connected)",
                          req.symbol, req.volume),
              WARNING);
    return "";
}

void NullGateway::cancel_order(const utilities::CancelRequest& /*req*/) {}

void NullGateway::query_account() {}

void NullGateway::query_position() {}

void NullGateway::process_timer_event(const utilities::Event& /*event*/) {}

} // namespace engines
