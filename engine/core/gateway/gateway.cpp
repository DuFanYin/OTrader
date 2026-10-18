#include "core/gateway/gateway.hpp"
#include <format>
#include <stdexcept>
#include <utility>

namespace core {

Gateway::Gateway(utilities::Logger* logger, const std::string& name)
    : log_(logger, name) {}

void Gateway::set_callbacks(OrderCallback on_order, TradeCallback on_fill) {
    on_order_ = std::move(on_order);
    on_trade_ = std::move(on_fill);
}

void Gateway::put_order(const model::Order& order) const {
    if (on_order_) {
        on_order_(order);
    }
}

void Gateway::put_fill(const model::Fill& fill) const {
    if (on_trade_) {
        on_trade_(fill);
    }
}

void Gateway::connect() {
    log_.write_log("No broker in this build (BUILD_GATEWAY=OFF): orders will be rejected",
                   utilities::WARNING);
}

auto Gateway::send_order(const model::OrderRequest& req) -> std::string {
    log_.write_log(std::format("No broker: rejecting order symbol={} vol={}", req.symbol,
                               req.quantity),
                   utilities::WARNING);
    return "";
}

void Gateway::cancel_order(const model::CancelRequest& /*req*/) {}

auto Gateway::query_account() -> std::future<BrokerAccount> {
    throw std::runtime_error("No broker in this build (BUILD_GATEWAY=OFF): no account to ask");
}

auto Gateway::sync_contracts(std::string_view /*portfolio*/, int /*max_dte*/)
    -> std::future<std::vector<model::Contract>> {
    throw std::runtime_error("No broker in this build (BUILD_GATEWAY=OFF): no contracts to sync");
}

auto Gateway::catch_up() -> std::future<void> {
    throw std::runtime_error(
        "No broker in this build (BUILD_GATEWAY=OFF): nothing to catch up with");
}

} // namespace core
