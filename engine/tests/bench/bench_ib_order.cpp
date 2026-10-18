/**
 * An order through the IB adapter to the wire: IbGateway connected to a TWS on a local port
 * (tests/fake_tws.hpp), which stamps each placeOrder as it reads it off its socket.
 *
 * Paths, each timed with the TSC (PER_OP, ns):
 *   send_order          what IbGateway::send_order costs the thread that calls it (live: the event
 *                       thread): the IB contract and order built, the order's report queued, the
 *                       order on its way
 *   send_order → TWS    from the call to the TWS having read the order: the hand-off, if any, to
 *                       IB's message thread, the API's encoding, the socket
 * idle: one at a time with a pause between, so IB's message thread sleeps in between (as between
 * quotes live; the calling thread spins, as the event thread is awake when it sends); back to
 * back: the next one as soon as the TWS has the last. The TWS answers nothing.
 */

#include "tests/bench/bench_util.hpp"
#include "tests/fake_tws.hpp"

#include "adapters/gateway/ib_gateway.hpp"
#include "model/object.hpp"
#include "model/order_builder.hpp"
#include "model/symbol.hpp"
#include "utilities/logger.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

/** Drops every line: what logging costs the caller is the log engine's, measured elsewhere. */
struct Quiet final : utilities::Logger {
    void log(const utilities::LogRecord& /*record*/) override {}
};

/** The TSC when the TWS read the latest placeOrder, and how many it has read. */
struct Arrivals {
    std::atomic<std::uint64_t> at{0};
    std::atomic<std::uint64_t> count{0};
};

void run(adapters::IbGateway& gateway, const model::OrderRequest& req, Arrivals& arrivals,
         double pause_ns, int samples, const char* title) {
    bench::Histogram call;
    bench::Histogram wire;
    for (int i = 0; i < samples + 200; ++i) { // the first 200 warm up
        const std::uint64_t before = arrivals.count.load(std::memory_order_acquire);
        const std::uint64_t t0 = bench::tsc_ticks();
        bench::keep(gateway.send_order(req));
        const std::uint64_t t1 = bench::tsc_ticks();
        while (arrivals.count.load(std::memory_order_acquire) == before) {
            bench::cpu_relax();
        }
        const std::uint64_t t2 = arrivals.at.load(std::memory_order_acquire);
        if (i >= 200) {
            call.record_ns(bench::ticks_to_ns(t1 - t0));
            wire.record_ns(bench::ticks_to_ns(t2 - t0));
        }
        if (pause_ns > 0) {
            bench::spin_for_ns(pause_ns);
        }
    }
    bench::print_header(title);
    bench::print_row("send_order", call);
    bench::print_row("send_order -> TWS has it", wire);
}

} // namespace

int main() {
    bench::print_env();
    if (!bench::tsc_usable()) {
        std::printf("no usable TSC\n");
        return 1;
    }
    Arrivals arrivals;
    tests::FakeTws tws("DU1", [&arrivals](const std::vector<std::string>& fields) {
        if (!fields.empty() && fields[0] == std::to_string(tests::FakeTws::kPlaceOrder)) {
            arrivals.at.store(bench::tsc_ticks(), std::memory_order_relaxed);
            arrivals.count.fetch_add(1, std::memory_order_release);
        }
    });
    Quiet quiet;
    adapters::IbGateway gateway(&quiet);
    gateway.set_callbacks([](const model::Order&) {}, [](const model::Fill&) {});
    adapters::IbSettings& settings = gateway.default_setting();
    settings.host = "127.0.0.1";
    settings.port = tws.port();
    settings.account = "DU1";
    gateway.connect();

    using namespace std::chrono;
    const std::string call = model::option_symbol(
        {.root = "SPY", .expiry = 2099y / December / 18, .type = model::OptionType::CALL,
         .strike = 600});
    const std::array leg{model::Leg{.ratio = 1, .side = model::Side::BUY, .symbol = call}};
    const model::OrderRequest req = model::order_request(leg, 1.25, 1, model::OrderType::LIMIT);

    run(gateway, req, arrivals, 300'000.0, 2000, "IB order, idle (300 us between)");
    run(gateway, req, arrivals, 0.0, 2000, "IB order, back to back");
    gateway.close();
    return 0;
}
