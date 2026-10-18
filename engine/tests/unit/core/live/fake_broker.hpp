#pragma once

/** A stand-in for a broker at the engine's venue port: it answers at once, from what the test
 * sets; or, with sync_hangs, a sync not at all until it is disconnected (as IB that stopped
 * answering). */

#include "core/gateway/gateway.hpp"
#include <atomic>
#include <cstddef>
#include <exception>
#include <future>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace optra_tests {

struct FakeBroker final : core::Gateway {
    FakeBroker() : Gateway(nullptr, "FakeBroker") {}

    void connect() override {
        if (refuse_connect) {
            throw std::runtime_error("the gateway is not up");
        }
        connected = true;
    }
    void disconnect() override {
        connected = false;
        const std::lock_guard lock(hanging_mutex);
        if (hanging) { // as IbGateway fails what it was asked when it disconnects
            hanging->set_exception(std::make_exception_ptr(std::runtime_error("disconnected")));
            hanging.reset();
        }
    }
    [[nodiscard]] bool is_connected() const override { return connected; }
    [[nodiscard]] bool has_broker() const override { return true; }
    std::string send_order(const model::OrderRequest& req) override {
        sent.push_back(req.symbol);
        return "7";
    }
    void cancel_order(const model::CancelRequest& /*req*/) override {}
    std::future<core::BrokerAccount> query_account() override {
        std::promise<core::BrokerAccount> done;
        done.set_value(account);
        return done.get_future();
    }
    void restore_orders(std::span<const model::Order> orders) override {
        restored = orders.size();
    }
    std::future<std::vector<model::Contract>> sync_contracts(std::string_view /*portfolio*/,
                                                             int /*max_dte*/) override {
        std::promise<std::vector<model::Contract>> done;
        if (sync_hangs) {
            const std::lock_guard lock(hanging_mutex);
            auto future = done.get_future();
            hanging = std::move(done);
            return future;
        }
        if (sync_fails) {
            done.set_exception(std::make_exception_ptr(
                std::runtime_error("Syncing contracts: IB did not answer within 60 s")));
        } else {
            done.set_value(contracts);
        }
        return done.get_future();
    }
    std::future<void> catch_up() override {
        ++catch_ups;
        std::promise<void> done;
        if (catch_up_fails) {
            done.set_exception(std::make_exception_ptr(
                std::runtime_error("Catching up with IB: IB did not answer within 60 s")));
        } else {
            done.set_value();
        }
        return done.get_future();
    }

    std::atomic<bool> connected{false};
    std::atomic<bool> refuse_connect{false}; // as a gateway not up yet, or not logged in
    std::vector<std::string> sent;
    std::size_t restored = 0;
    int catch_ups = 0;
    bool catch_up_fails = false;
    core::BrokerAccount account; // what query_account answers
    std::vector<model::Contract> contracts; // what sync_contracts answers
    std::atomic<bool> sync_fails{false}; // read on the trading days thread too
    std::atomic<bool> sync_hangs{false};
    std::mutex hanging_mutex;
    std::optional<std::promise<std::vector<model::Contract>>> hanging; // the sync not answered
};

} // namespace optra_tests
