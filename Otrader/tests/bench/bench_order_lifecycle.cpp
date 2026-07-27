/**
 * L4 — order lifecycle latency (engine-side, broker-free).
 *
 * Real trading systems care most about tick-to-trade: quote in → decide → order out → fill back.
 * The broker legs (ack/fill round-trip) need a live gateway, but the ENGINE-SIDE segments are
 * measurable now on the backtest MainEngine, whose EventEngine dispatches synchronously inline
 * (put_event → run_dispatch in the calling thread) and needs no DB/network (see benchmarkPlan §4b).
 *
 * Segments measured here (all PER_OP, µs-scale):
 *   - submit           : ExecutionEngine::send_order → risk check + register + in-process gateway ack.
 *                        This is the "strategy calls send_order" cost, minus any real broker.
 *   - dispatch(Order)  : put_event(Order) → execution.process_order_event + position + strategy cb.
 *                        The engine-side work an inbound Order status update triggers.
 *   - dispatch(Trade)  : put_event(Trade) → execution + position + strategy trade handling.
 *   - engine round-trip: submit + dispatch(Order) + dispatch(Trade) — the full engine-side path a
 *                        single filled order costs, excluding the external broker hop.
 *
 * The order/trade payloads are synthetic (fixed fields); we measure engine mechanics, not fills.
 */

#include "bench_util.hpp"

#include "engine_main.hpp" // backtest::MainEngine
#include "object.hpp"

#include <cstdint>
#include <cstdio>
#include <string>

using bench::Histogram;
using bench::keep;

namespace {

// A trivial in-process "gateway": assigns a monotonic orderid and returns it (mirrors the
// single-process IbGateway/NullGateway send_order returning an id, minus any real broker work).
struct BenchHarness {
    backtest::MainEngine engine;
    int next_id = 0;

    BenchHarness() {
        engine.set_order_executor([this](const utilities::OrderRequest&) -> std::string {
            return "O" + std::to_string(++next_id);
        });
    }

    static utilities::OrderRequest make_req() {
        utilities::OrderRequest req;
        req.symbol = "SPXW  250804C05000000";
        req.direction = utilities::Direction::LONG;
        req.type = utilities::OrderType::LIMIT;
        req.volume = 1;
        req.price = 10.0;
        req.reference = "bench";
        return req;
    }

    static utilities::OrderData make_order(const std::string& oid, utilities::Status st) {
        utilities::OrderData o;
        o.symbol = "SPXW  250804C05000000";
        o.orderid = oid;
        o.direction = utilities::Direction::LONG;
        o.type = utilities::OrderType::LIMIT;
        o.price = 10.0;
        o.volume = 1;
        o.traded = (st == utilities::Status::ALLTRADED) ? 1 : 0;
        o.status = st;
        return o;
    }

    static utilities::TradeData make_trade(const std::string& oid, int tid) {
        utilities::TradeData t;
        t.symbol = "SPXW  250804C05000000";
        t.orderid = oid;
        t.tradeid = "T" + std::to_string(tid);
        t.direction = utilities::Direction::LONG;
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
            utilities::OrderData* p = h.engine.acquire_order();
            if (p != nullptr) {
                *p = BenchHarness::make_order(oid, utilities::Status::NOTTRADED);
                h.engine.put_event(utilities::Event(utilities::EventType::Order, p));
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
            utilities::TradeData* p = h.engine.acquire_trade();
            if (p != nullptr) {
                *p = BenchHarness::make_trade(oid, ++tid);
                h.engine.put_event(utilities::Event(utilities::EventType::Trade, p));
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
            auto push_order = [&](utilities::Status st) {
                utilities::OrderData* p = h.engine.acquire_order();
                if (p != nullptr) {
                    *p = BenchHarness::make_order(oid, st);
                    h.engine.put_event(utilities::Event(utilities::EventType::Order, p));
                }
            };
            push_order(utilities::Status::NOTTRADED);
            push_order(utilities::Status::ALLTRADED);
            utilities::TradeData* tp = h.engine.acquire_trade();
            if (tp != nullptr) {
                *tp = BenchHarness::make_trade(oid, ++tid);
                h.engine.put_event(utilities::Event(utilities::EventType::Trade, tp));
            }
            keep(oid);
        },
        /*iters=*/10000, /*warmup=*/500);
    bench::print_row("engine round-trip (submit+2×Order+Trade)", hist);
}

} // namespace

int main() {
    bench::print_env();
    std::printf("NOTE: engine-side segments only (backtest sync engine, no broker). Broker ack/fill\n");
    std::printf("      round-trip needs a live gateway — see benchmarkPlan §4b.\n");

    bench::print_header("order lifecycle — engine-side segments (ns)");
    bench_submit();
    bench_dispatch_order();
    bench_dispatch_trade();
    bench_round_trip();

    return 0;
}
