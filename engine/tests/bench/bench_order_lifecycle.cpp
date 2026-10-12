/**
 * Order lifecycle latency (engine-side, broker-free).
 *
 * Real trading systems care most about tick-to-trade: quote in → decide → order out → fill back.
 * The broker legs (ack/fill round-trip) need a live gateway, but the ENGINE-SIDE segments are
 * measurable now on the backtest MainEngine, whose EventEngine dispatches synchronously inline
 * (put_event → process_event in the calling thread) and needs no network.
 *
 * Segments measured here (all PER_OP, µs-scale):
 *   - submit           : Engine::send_order → risk check + gateway ack + register.
 *                        This is the "strategy calls send_order" cost, minus any real broker.
 *   - dispatch(Order)  : put_event(Order) → execution.process_order_event + position + strategy cb.
 *                        The engine-side work an inbound Order status update triggers.
 *   - dispatch(Trade)  : put_event(Trade) → execution + position + strategy trade handling.
 *   - engine round-trip: submit + dispatch(Order) + dispatch(Trade) — the full engine-side path a
 *                        single filled order costs, excluding the external broker hop.
 *   - throughput       : orders/sec through the strategy-facing submit path, one thread.
 *
 * The live paths, through the event thread and the broker's thread, are bench_live_paths.
 *
 * The order/trade payloads are synthetic (fixed fields); we measure engine mechanics, not fills.
 */

#include "bench_util.hpp"

#include "core/engine_backtest.hpp" // core::backtest::MainEngine
#include "../../model/object.hpp"

#include <cstdint>
#include <cstdio>
#include <string>

using bench::Histogram;
using bench::keep;

namespace {

// The backtest engine and its venue (BacktestGateway): send_order queues the order and returns
// its id, as a gateway does, minus any real broker work.
struct BenchHarness {
    core::backtest::MainEngine engine;

    static model::OrderRequest make_req() {
        model::OrderRequest req;
        req.symbol = "SPXW-20250804-C-5000";
        req.direction = model::Direction::LONG;
        req.type = model::OrderType::LIMIT;
        req.volume = 1;
        req.price = 10.0;
        req.reference = "bench";
        return req;
    }

    static model::OrderData make_order(const std::string& oid, model::Status st) {
        model::OrderData o;
        o.symbol = "SPXW-20250804-C-5000";
        o.orderid = oid;
        o.direction = model::Direction::LONG;
        o.type = model::OrderType::LIMIT;
        o.price = 10.0;
        o.volume = 1;
        o.traded = (st == model::Status::ALLTRADED) ? 1 : 0;
        o.status = st;
        return o;
    }

    static model::TradeData make_trade(const std::string& oid, int tid) {
        model::TradeData t;
        t.symbol = "SPXW-20250804-C-5000";
        t.orderid = oid;
        t.tradeid = "T" + std::to_string(tid);
        t.direction = model::Direction::LONG;
        t.price = 10.0;
        t.volume = 1;
        return t;
    }
};

// submit: strategy-facing send_order (risk check + register + in-process ack).
void bench_submit() {
    BenchHarness h;
    Histogram hist = bench::measure_per_op(
        [&] {
            std::string oid = h.engine.send_order("bench_strat", BenchHarness::make_req());
            keep(oid);
        },
        /*iters=*/20000, /*warmup=*/500);
    bench::print_row("submit (send_order → ack)", hist);
}

// dispatch(Order): put_event(Order) → full inline dispatch (execution + position + strategy).
void bench_dispatch_order() {
    BenchHarness h;
    // Pre-register an order so dispatch takes the "known order" path.
    std::string oid = h.engine.send_order("bench_strat", BenchHarness::make_req());
    Histogram hist = bench::measure_per_op(
        [&] {
            model::OrderData* p = h.engine.acquire_order();
            if (p != nullptr) {
                *p = BenchHarness::make_order(oid, model::Status::NOTTRADED);
                h.engine.put_event(model::Event(model::EventType::Order, p));
            }
            keep(p);
        },
        /*iters=*/20000, /*warmup=*/500);
    bench::print_row("dispatch(Order status update)", hist);
}

// dispatch(Trade): put_event(Trade) → full inline dispatch.
void bench_dispatch_trade() {
    BenchHarness h;
    std::string oid = h.engine.send_order("bench_strat", BenchHarness::make_req());
    int tid = 0;
    Histogram hist = bench::measure_per_op(
        [&] {
            model::TradeData* p = h.engine.acquire_trade();
            if (p != nullptr) {
                *p = BenchHarness::make_trade(oid, ++tid);
                h.engine.put_event(model::Event(model::EventType::Trade, p));
            }
            keep(p);
        },
        /*iters=*/20000, /*warmup=*/500);
    bench::print_row("dispatch(Trade)", hist);
}

// engine round-trip: submit → Order(NOTTRADED) → Order(ALLTRADED) → Trade, the full engine-side
// path a single filled order costs (no external broker hop).
void bench_round_trip() {
    BenchHarness h;
    int tid = 0;
    Histogram hist = bench::measure_per_op(
        [&] {
            std::string oid = h.engine.send_order("bench_strat", BenchHarness::make_req());
            auto push_order = [&](model::Status st) {
                model::OrderData* p = h.engine.acquire_order();
                if (p != nullptr) {
                    *p = BenchHarness::make_order(oid, st);
                    h.engine.put_event(model::Event(model::EventType::Order, p));
                }
            };
            push_order(model::Status::NOTTRADED);
            push_order(model::Status::ALLTRADED);
            model::TradeData* tp = h.engine.acquire_trade();
            if (tp != nullptr) {
                *tp = BenchHarness::make_trade(oid, ++tid);
                h.engine.put_event(model::Event(model::EventType::Trade, tp));
            }
            keep(oid);
        },
        /*iters=*/10000, /*warmup=*/500);
    bench::print_row("engine round-trip (submit+2×Order+Trade)", hist);
}

// throughput: orders/sec through the strategy-facing submit path (one thread).
void bench_throughput() {
    BenchHarness h;
    const model::OrderRequest req = BenchHarness::make_req();
    const uint64_t n = 500000;
    for (uint64_t i = 0; i < 5000; ++i) keep(h.engine.send_order("s", req)); // warmup
    const uint64_t t0 = bench::clock_ticks();
    for (uint64_t i = 0; i < n; ++i) keep(h.engine.send_order("s", req));
    const double ns = bench::ticks_to_ns(bench::clock_ticks() - t0);
    std::printf("submit throughput (1 thread) : %.0f orders/sec  (%.0f ns/order)\n",
                static_cast<double>(n) * 1e9 / ns, ns / static_cast<double>(n));
}

} // namespace

int main() {
    bench::print_env();
    std::printf("NOTE: engine-side segments only (backtest sync engine, no broker). Broker ack/fill\n");
    std::printf("      round-trip through the threads: bench_live_paths.\n");

    bench::print_header("order lifecycle — engine-side segments (ns)");
    bench_submit();
    bench_dispatch_order();
    bench_dispatch_trade();
    bench_round_trip();

    std::printf("\n== throughput ==\n");
    bench_throughput();
    return 0;
}
