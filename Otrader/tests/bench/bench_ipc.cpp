/**
 * L3 — live IPC latency (gateway REQ/REP over ZMQ).
 *
 * A mock gateway thread binds a ZMQ REP socket and echoes an orderid for each send_order request.
 * We time the client REQ/REP roundtrip two ways:
 *
 *   - NEW-SOCKET-PER-CALL : exactly what GatewayClient::req_rep does today (latencyFindings.md
 *                           F-3) — construct zmq::context_t + REQ socket + connect every call.
 *   - PERSISTENT-SOCKET   : one context + REQ socket reused across calls (the proposed fix).
 *
 * The delta between the two is the cost F-3 is paying per order. PER_OP timing (µs-scale).
 * Payload is a real-sized serialized OrderRequest (§0.1: IPC latency depends on payload SIZE,
 * not content).
 */

#include "bench_util.hpp"

#include "zmq_gateway_schema.hpp"
#include "object.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <unistd.h> // getpid — unique ipc:// path avoids port/address collisions across runs

#include <zmq.hpp>

using bench::clock_ticks;
using bench::Histogram;
using bench::ticks_to_ns;

namespace {

constexpr const char* kAddr = "inproc://bench_gw"; // inproc: pure IPC-stack cost, no TCP/loopback

// Mock gateway: REP socket, decodes request, replies with orderid on send_order. Runs until stop.
// Blocking REP loop with a short recv timeout (so it can observe `stop`). Blocking recv removes
// the busy-poll/yield noise, so the client roundtrip measures ZMQ + serialization, not our poll.
void mock_gateway_loop(zmq::socket_t& rep, std::atomic<bool>& stop) {
    rep.set(zmq::sockopt::rcvtimeo, 50); // ms
    rep.set(zmq::sockopt::linger, 0);
    uint64_t seq = 0;
    while (!stop.load(std::memory_order_acquire)) {
        zmq::message_t req;
        zmq::recv_result_t rc;
        try {
            rc = rep.recv(req, zmq::recv_flags::none);
        } catch (...) {
            break;
        }
        if (!rc) continue; // timeout → re-check stop
        std::string bytes(req.data<char>(), req.size());
        auto parsed = engines::request_deserialize(bytes);
        std::string resp = (parsed && parsed->cmd == engines::ZMQ_CMD_SEND_ORDER)
                               ? engines::response_serialize("OID-" + std::to_string(++seq))
                               : engines::response_serialize_ok();
        rep.send(zmq::buffer(resp), zmq::send_flags::none);
    }
}

void mock_gateway(zmq::context_t& ctx, std::atomic<bool>& ready, std::atomic<bool>& stop) {
    zmq::socket_t rep(ctx, ZMQ_REP);
    rep.bind(kAddr);
    ready.store(true, std::memory_order_release);
    mock_gateway_loop(rep, stop);
}

// Build a realistic send_order request payload once.
std::string make_send_order_request() {
    utilities::OrderRequest r;
    r.symbol = "SPXW  250804C05000000";
    r.exchange = utilities::Exchange::CBOE;
    r.direction = utilities::Direction::LONG;
    r.type = utilities::OrderType::LIMIT;
    r.price = 12.34;
    r.volume = 1;
    return engines::request_serialize(engines::ZMQ_CMD_SEND_ORDER,
                                      engines::order_request_serialize(r));
}

// Variant A: new context + socket every call (current GatewayClient::req_rep).
Histogram bench_new_socket(const char* addr, const std::string& req_bytes, int iters, int warmup) {
    auto one = [&] {
        zmq::context_t ctx(1);
        zmq::socket_t req(ctx, ZMQ_REQ);
        req.set(zmq::sockopt::linger, 0); // don't block on ctx destruction
        req.connect(addr);
        // NOTE: inproc requires the SAME context as the binder — new-context can't reach an
        // inproc bind. So the new-socket variant is measured over tcp loopback (see main()).
        req.send(zmq::buffer(req_bytes), zmq::send_flags::none);
        zmq::message_t reply;
        (void)req.recv(reply, zmq::recv_flags::none);
    };
    for (int i = 0; i < warmup; ++i) one();
    Histogram h;
    for (int i = 0; i < iters; ++i) {
        uint64_t t0 = clock_ticks();
        one();
        uint64_t t1 = clock_ticks();
        h.record_ns(ticks_to_ns(t1 - t0));
    }
    return h;
}

// Variant B: persistent context + socket reused across calls (proposed fix).
Histogram bench_persistent_socket(zmq::context_t& ctx, const char* addr, const std::string& req_bytes,
                                  int iters, int warmup) {
    zmq::socket_t req(ctx, ZMQ_REQ);
    req.set(zmq::sockopt::linger, 0);
    req.connect(addr);
    auto one = [&] {
        req.send(zmq::buffer(req_bytes), zmq::send_flags::none);
        zmq::message_t reply;
        (void)req.recv(reply, zmq::recv_flags::none);
    };
    for (int i = 0; i < warmup; ++i) one();
    Histogram h;
    for (int i = 0; i < iters; ++i) {
        uint64_t t0 = clock_ticks();
        one();
        uint64_t t1 = clock_ticks();
        h.record_ns(ticks_to_ns(t1 - t0));
    }
    return h;
}

} // namespace

int main() {
    bench::print_env();
    const std::string req_bytes = make_send_order_request();
    std::printf("send_order request payload: %zu bytes\n", req_bytes.size());
    constexpr int kIters = 100000;
    constexpr int kWarmup = 2000;

    // --- Persistent socket over inproc (shared context) ---
    {
        zmq::context_t ctx(1);
        std::atomic<bool> ready{false}, stop{false};
        std::thread gw(mock_gateway, std::ref(ctx), std::ref(ready), std::ref(stop));
        while (!ready.load(std::memory_order_acquire)) {}
        Histogram h = bench_persistent_socket(ctx, kAddr, req_bytes, kIters, kWarmup);
        stop.store(true, std::memory_order_release);
        gw.join();
        bench::print_header("L3 gateway REQ/REP — PERSISTENT socket (inproc)");
        bench::print_row("send_order roundtrip", h);
    }

    // --- New-socket-per-call vs persistent, both over TCP loopback (fair comparison: new context
    //     can't share an inproc transport, so both variants use tcp here). This isolates the cost
    //     of context+socket construction per call (F-3). ---
    {
        const char* tcp_addr = "tcp://127.0.0.1:5761";
        zmq::context_t ctx(1);
        std::atomic<bool> ready{false}, stop{false};
        std::thread gw([&] {
            zmq::socket_t rep(ctx, ZMQ_REP);
            rep.bind(tcp_addr);
            ready.store(true, std::memory_order_release);
            mock_gateway_loop(rep, stop);
        });
        while (!ready.load(std::memory_order_acquire)) {}

        // fewer iters for new-socket: each call builds+destroys a context (expensive by design).
        Histogram hnew = bench_new_socket(tcp_addr, req_bytes, 2000, 100);
        Histogram hpers = bench_persistent_socket(ctx, tcp_addr, req_bytes, 50000, 1000);

        stop.store(true, std::memory_order_release);
        gw.join();

        bench::print_header("L3 gateway REQ/REP — TCP loopback: new-socket (F-3) vs persistent");
        bench::print_row("new context+socket per call (F-3)", hnew);
        bench::print_row("persistent socket", hpers);
    }

    return 0;
}
