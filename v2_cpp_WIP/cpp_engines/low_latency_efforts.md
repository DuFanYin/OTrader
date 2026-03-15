# Otrader Low-Latency Design Overview

Otrader applies targeted optimizations in its event-driven, multi-threaded runtime—around queues, memory, and copy paths—to reduce latency, improve throughput, and keep response times stable. This document summarizes these design choices from an external perspective, without implementation details.

---

## Design Goals

- **Lower tail latency**: Reduce lock contention and allocations on hot paths so that event processing latency is more predictable.
- **Higher throughput**: Under high-frequency market, order, and trade data, avoid allocation and copying from becoming the bottleneck.
- **Extensibility**: Live and backtest share the same event and data model, making it easier to tune performance while keeping behavior consistent.

---

## 1. Lock-Free Queues

**What we do**  
The live event engine uses **lock-free bounded ring buffers** for the main event stream and the strategy event stream:  
- The main event stream is **multi-producer, single-consumer**: market data, gateway, and timer threads enqueue; a single main worker consumes.  
- The strategy event stream is **single-producer, single-consumer**: the main worker enqueues; the strategy thread consumes.

**Benefits**  
- Normal enqueue/dequeue does not rely on mutexes, reducing lock contention and latency spikes.  
- Fixed, pre-allocated capacity avoids the unpredictable cost of runtime growth.  
- Behavior when the queue is full (retry or drop) can be tuned to balance latency and reliability.

---

## 2. Multi-Threading and Event Pipelines

**What we do**  
The live runtime uses a **pipelined thread layout**:  
- **Main worker thread**: Consumes the main event queue; handles market snapshots, order and trade callbacks, and runs infrastructure (position, hedging, etc.).  
- **Strategy thread**: Has its own strategy event queue; runs only strategy callbacks (timer, order, trade), decoupled from the main thread.  
- **Timer thread**: Drives periodic timer events.  

Events move between the main and strategy queues via lock-free rings. Threads block on a condition variable when a queue is empty and consume as soon as data is available, avoiding busy-wait.

**Benefits**  
- Strategy logic is separated from infrastructure (position, risk, gateway); one slow strategy does not stall the whole pipeline.  
- The path from enqueue to strategy consumption is shorter and more predictable, which helps control end-to-end latency.

---

## 3. Object Pools and Reuse

**What we do**  
High-allocation objects—events, orders, trades, snapshots, logs, strategy state—use a shared **object pool** pattern:  
- A batch of objects is pre-allocated; callers acquire from the pool, fill, hand off, and then release back to the pool instead of per-call `new`/`delete`.  
- Pool growth and concurrent access follow clear rules to avoid double-release and races during shutdown.

**Benefits**  
- Heap allocation and deallocation on hot paths drop sharply, reducing allocator contention and fragmentation.  
- Live and backtest share the same acquire–use–release discipline for events, orders, trades, and snapshots, so behavior stays consistent and maintainable.

---

## 4. Zero-Copy and Pointer Passing

**What we do**  
- **Event payloads**: “Heavy” data (orders, trades, portfolio snapshots) are passed by **pointer** in events; no second full copy inside the event type. Pointers come from the object pool and are released by the consumer after processing.  
- **Backtest data**: Columnar timestep data is exposed as **read-only views**; consumers index into column arrays by row without per-row copies or extra allocations inside the timestep loop.

**Benefits**  
- Large copies are removed from the event path, cutting CPU and memory bandwidth use.  
- Backtest keeps memory usage and latency stable when scanning long histories and many symbols.

---

## 5. Move Semantics for Event Delivery

**What we do**  
The event delivery API supports **move-by-value** and **lvalue-to-move**:  
- Callers can build a temporary event and pass it in; the implementation moves it into the queue or pool without an extra copy.  
- If an event is built first and then passed as an lvalue, the API turns that into a single move, still avoiding a copy.

**Benefits**  
- Regardless of calling style, event data is only moved on the hot path, never copied. Together with pointer payloads, this completes the low-copy / zero-copy design for the event path.

---

## 6. Precomputation (Backtest)

**What we do**  
In backtest, portfolio snapshots are **built once** in a single pass over the columnar data, then stored and reused: each timestep applies a precomputed snapshot by index instead of rebuilding from the frame. No per-step snapshot construction on the hot path.

**Benefits**  
- Removes repeated work from the inner timestep loop; backtest runtime scales better with history length and symbol count.  
- Pairs with the snapshot object pool and columnar views so that apply-by-index is cheap and allocation-free.

---

## 7. Parallel Backtest

**What we do**  
When running over multiple files (e.g. one parquet per day or per symbol), the backtest entry can run **several workers in parallel**: each worker pulls a file from a shared queue, runs a full backtest for that file in isolation, then pushes results. Workers use a shared stop token so one failure can cancel the rest; results are merged in a defined order.

**Benefits**  
- Wall-clock time for multi-file backtests drops (up to the number of workers), improving iteration speed.  
- Per-file state is isolated (no shared engine), so the model stays simple and cache-friendly.

---

## 8. Other Practices

- **Non-owning views**: APIs that take lists of symbols, legs, or options use **span-like views** where possible so callers can pass contiguous data without copying.  
- **Capacity hints**: Containers that are filled in a loop often **reserve** the expected size up front to avoid reallocations and keep the hot path stable.  
- **Cache-friendly layout**: Shared ring buffers use cache-line separation for producer and consumer indices to reduce false sharing between threads.

---

## Summary

| Area              | Approach                                      | Main benefit                    |
|-------------------|-----------------------------------------------|---------------------------------|
| Lock-free queues  | Main stream MPSC, strategy stream SPSC rings  | Less lock contention, stable latency |
| Event pipeline    | Main + strategy + timer threads               | Strategy decoupled from infra   |
| Object pools      | Reuse for events, orders, trades, snapshots, logs | Fewer heap allocations, higher throughput |
| Zero-copy         | Pointer payloads, columnar views in backtest  | Fewer copies, less bandwidth    |
| Move semantics    | Move and lvalue-to-move for event delivery    | No redundant copies on event path |
| Precomputation    | One-pass snapshot build, apply by index (backtest) | Less work per timestep          |
| Parallel backtest| Multi-worker, per-file isolation              | Shorter wall-clock for multi-file runs |
| Other             | Spans, reserve(), cache-line alignment        | Fewer copies/reallocs, less false sharing |

These designs are in place for both live and backtest where applicable and continue to evolve (e.g. per-strategy threads). For implementation details and extension points, see the project’s technical design docs.
