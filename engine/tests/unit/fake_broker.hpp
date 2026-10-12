#pragma once

/** A stand-in for a broker at the engine's venue port: it answers at once, from what the test
 * sets. */

#include "adapters/broker_account.hpp"
#include "adapters/engine_gateway.hpp"
#include <cstddef>
#include <exception>
#include <future>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace optra_tests {

struct FakeBroker final : adapters::Gateway {
    FakeBroker() : Gateway(nullptr, "FakeBroker") {}

    void connect() override { connected = true; }
    void disconnect() override { connected = false; }
    [[nodiscard]] bool is_connected() const override { return connected; }
    [[nodiscard]] bool has_broker() const override { return true; }
    std::string send_order(const model::OrderRequest& req) override {
        sent.push_back(req.symbol);
        return "7";
    }
    void cancel_order(const model::CancelRequest& /*req*/) override {}
    std::future<adapters::BrokerAccount> query_account() override {
        std::promise<adapters::BrokerAccount> done;
        done.set_value(account);
        return done.get_future();
    }
    void restore_orders(std::span<const model::OrderData> orders) override {
        restored = orders.size();
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

    bool connected = false;
    std::vector<std::string> sent;
    std::size_t restored = 0;
    int catch_ups = 0;
    bool catch_up_fails = false;
    adapters::BrokerAccount account; // what query_account answers
};

} // namespace optra_tests
