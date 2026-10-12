#include "engine_gateway.hpp"
#include <format>
#include <stdexcept>
#include <utility>

namespace adapters {

Gateway::Gateway(utilities::Logger* logger, const std::string& name)
    : utilities::BaseEngine(logger, name) {}

void Gateway::set_callbacks(OrderCallback on_order, TradeCallback on_trade) {
    on_order_ = std::move(on_order);
    on_trade_ = std::move(on_trade);
}

void Gateway::put_order(const model::OrderData& order) const {
    if (on_order_) {
        on_order_(order);
    }
}

void Gateway::put_trade(const model::TradeData& trade) const {
    if (on_trade_) {
        on_trade_(trade);
    }
}

void Gateway::connect() {
    write_log("No broker in this build (BUILD_GATEWAY=OFF): orders will be rejected",
              utilities::WARNING);
}

auto Gateway::send_order(const model::OrderRequest& req) -> std::string {
    write_log(std::format("No broker: rejecting order symbol={} vol={}", req.symbol, req.volume),
              utilities::WARNING);
    return "";
}

void Gateway::cancel_order(const model::CancelRequest& /*req*/) {}

auto Gateway::query_account() -> std::future<BrokerAccount> {
    throw std::runtime_error("No broker in this build (BUILD_GATEWAY=OFF): no account to ask");
}

auto Gateway::sync_contracts(std::string_view /*portfolio*/, int /*max_dte*/)
    -> std::future<std::vector<model::ContractData>> {
    throw std::runtime_error("No broker in this build (BUILD_GATEWAY=OFF): no contracts to sync");
}

auto Gateway::catch_up() -> std::future<void> {
    throw std::runtime_error(
        "No broker in this build (BUILD_GATEWAY=OFF): nothing to catch up with");
}

} // namespace adapters
