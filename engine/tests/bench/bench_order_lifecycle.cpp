/**
 * Order lifecycle latency (engine-side, broker-free).
 *
 * Real trading systems care most about tick-to-trade: quote in → decide → order out → fill back.
 * The broker legs (ack/fill round-trip) need a live gateway, but the ENGINE-SIDE segments are
 * measurable now on the backtest Main, whose EventLoop dispatches synchronously inline
 * (put_event → process_event in the calling thread) and needs no network.
 *
 * Segments measured here (all PER_OP, µs-scale):
 *   - submit           : Main::send_order → risk check + gateway ack + register.
 *                        This is the "strategy calls send_order" cost, minus any real broker.
 *   - dispatch(Order)  : put_event(Order) → execution.update_order + position + strategy cb.
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

#include "tests/lone_backtest.hpp"
#include "tests/bench/bench_util.hpp"

#include "model/object.hpp"

#include <cstdint>
#include <cstdio>
#include <string>

using bench::Histogram;
using bench::keep;

namespace {

// The backtest engine and its venue (BacktestGateway): send_order queues the order and returns
// its id, as a gateway does, minus any real broker work.
struct BenchHarness {
    test::LoneBacktest backtest;
    core::Main& engine = backtest.main();
    const model::StrategyRecord strategy{.name = "bench_strat"};

    static model::OrderRequest make_req() {
        model::OrderRequest req;
        req.symbol = "SPXW-20250804-C-5000";
        req.legs = {{.ratio = 1, .side = model::Side::BUY, .symbol = req.symbol}};
        req.type = model::OrderType::LIMIT;
        req.quantity = 1;
        req.price = 10.0;
        req.reference = "bench";
        return req;
    }

    static model::Order make_order(const std::string& oid, model::Status st) {
        model::Order o;
        o.symbol = "SPXW-20250804-C-5000";
        o.order_id = oid;
        o.legs = {{.ratio = 1, .side = model::Side::BUY, .symbol = o.symbol}};
        o.type = model::OrderType::LIMIT;
        o.price = 10.0;
        o.quantity = 1;
        o.filled = (st == model::Status::FILLED) ? 1 : 0;
        o.status = st;
        return o;
    }

    static model::Fill make_trade(const std::string& oid, int tid) {
        model::Fill t;
        t.symbol = "SPXW-20250804-C-5000";
        t.order_id = oid;
        t.fill_id = "T" + std::to_string(tid);
        t.side = model::Side::BUY;
        t.price = 10.0;
        t.quantity = 1;
        return t;
    }
};

// submit: strategy-facing send_order (risk check + register + in-process ack).
void bench_submit() {
    BenchHarness h;
    Histogram hist = bench::measure_per_op(
        [&] {
            std::string oid = h.engine.send_order(h.strategy, BenchHarness::make_req());
            keep(oid);
        },
        /*iters=*/20000, /*warmup=*/500);
    bench::print_row("submit, orders left working (to 20k)", hist);
}

// submit as live: ten orders working, each one sent then cancelled outside the timing, so the
// working set stays at ten (the row above lets them pile up: the risk check walks a strategy's
// working orders, so its cost grows with them).
void bench_submit_ten_working() {
    BenchHarness h;
    for (int i = 0; i < 10; ++i) {
        keep(h.engine.send_order(h.strategy, BenchHarness::make_req()));
    }
    Histogram hist;
    for (int i = 0; i < 20500; ++i) {
        const std::uint64_t t0 = bench::clock_ticks();
        std::string oid = h.engine.send_order(h.strategy, BenchHarness::make_req());
        const std::uint64_t t1 = bench::clock_ticks();
        if (i >= 500) {
            hist.record_ns(bench::ticks_to_ns(t1 - t0));
        }
        if (model::Order* p = h.engine.event_loop()->acquire_order(); p != nullptr) {
            *p = BenchHarness::make_order(oid, model::Status::CANCELLED);
            h.engine.event_loop()->put_event(model::Event(model::EventType::Order, p));
        }
    }
    bench::print_row("submit, ten orders working", hist);
}

// dispatch(Order): put_event(Order) → full inline dispatch (execution + position + strategy).
void bench_dispatch_order() {
    BenchHarness h;
    // Pre-register an order so dispatch takes the "known order" path.
    std::string oid = h.engine.send_order(h.strategy, BenchHarness::make_req());
    Histogram hist = bench::measure_per_op(
        [&] {
            model::Order* p = h.engine.event_loop()->acquire_order();
            if (p != nullptr) {
                *p = BenchHarness::make_order(oid, model::Status::WORKING);
                h.engine.event_loop()->put_event(model::Event(model::EventType::Order, p));
            }
            keep(p);
        },
        /*iters=*/20000, /*warmup=*/500);
    bench::print_row("dispatch(Order status update)", hist);
}

// dispatch(Trade): put_event(Trade) → full inline dispatch.
void bench_dispatch_trade() {
    BenchHarness h;
    std::string oid = h.engine.send_order(h.strategy, BenchHarness::make_req());
    int tid = 0;
    Histogram hist = bench::measure_per_op(
        [&] {
            model::Fill* p = h.engine.event_loop()->acquire_fill();
            if (p != nullptr) {
                *p = BenchHarness::make_trade(oid, ++tid);
                h.engine.event_loop()->put_event(model::Event(model::EventType::Fill, p));
            }
            keep(p);
        },
        /*iters=*/20000, /*warmup=*/500);
    bench::print_row("dispatch(Trade)", hist);
}

// engine round-trip: submit → Order(WORKING) → Order(FILLED) → Trade, the full engine-side
// path a single filled order costs (no external broker hop).
void bench_round_trip() {
    BenchHarness h;
    int tid = 0;
    Histogram hist = bench::measure_per_op(
        [&] {
            std::string oid = h.engine.send_order(h.strategy, BenchHarness::make_req());
            auto push_order = [&](model::Status st) {
                model::Order* p = h.engine.event_loop()->acquire_order();
                if (p != nullptr) {
                    *p = BenchHarness::make_order(oid, st);
                    h.engine.event_loop()->put_event(model::Event(model::EventType::Order, p));
                }
            };
            push_order(model::Status::WORKING);
            push_order(model::Status::FILLED);
            model::Fill* tp = h.engine.event_loop()->acquire_fill();
            if (tp != nullptr) {
                *tp = BenchHarness::make_trade(oid, ++tid);
                h.engine.event_loop()->put_event(model::Event(model::EventType::Fill, tp));
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
    for (uint64_t i = 0; i < 5000; ++i) keep(h.engine.send_order(h.strategy, req)); // warmup
    const uint64_t t0 = bench::clock_ticks();
    for (uint64_t i = 0; i < n; ++i) keep(h.engine.send_order(h.strategy, req));
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
    bench_submit_ten_working();
    bench_dispatch_order();
    bench_dispatch_trade();
    bench_round_trip();

    std::printf("\n== throughput ==\n");
    bench_throughput();
    return 0;
}
