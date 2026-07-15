## Otrader Zero-Copy / Perfect-Forward Data Path

This note describes the **current** data path in `runtime/` and clarifies where Otrader still
achieves “zero-copy, perfect-forward” behavior, and where copies are intentionally introduced
(serialization, persistence, etc.).

The focus is on:

- **Live runtime**: `runtime/live`
- **Backtest runtime**: `runtime/backtest`

and how they interact with:

- `EventEngine` (queue + dispatch)
- `MainEngine` (live/backtest)
- Core engines: `ExecutionEngine`, `PositionEngine`, `OptionStrategyEngine`

---

## 1. Core idea: pointer payloads + pooled events

Across both live and backtest, the core pattern is:

- Heavy payloads (`PortfolioSnapshot`, `OrderData`, `TradeData`, `StrategyUpdateData`, etc.) are
  allocated via **object pools**:

  - Live: `engines::EventEngine`  
    `event_pool_`, `snapshot_pool_`, `order_pool_`, `trade_pool_`

  - Backtest: `backtest::EventEngine`  
    `event_pool_`, `snapshot_pool_`, `order_pool_`, `trade_pool_`

- Producers acquire payloads from the pool:

  - Live:
    - `engines::MainEngine::acquire_snapshot/order/trade()` forwards to
      `engines::EventEngine::acquire_*()`
    - `MarketDataClient` / `GatewayClient` call `main->acquire_*()` and then
      `main->put_event(Event(EventType::X, p))`

  - Backtest:
    - `backtest::MainEngine::acquire_*()` forwards to `backtest::EventEngine::acquire_*()`
    - `BacktestEngine` calls `main_engine_->acquire_snapshot()` and then
      `main_engine_->put_event(Event(EventType::Snapshot, snap))`

- The `Event` itself carries **only a pointer** to the heavy object:

  ```c++
  utilities::Event(EventType::Order, utilities::OrderData* p);
  ```

- Consumers (dispatchers) call the relevant engine(s) (`ExecutionEngine`, `PositionEngine`,
  `OptionStrategyEngine`) with **references to the pooled payload**, and then release back to the
  pool when the event’s lifetime is over.

This means the “hot event path” is **pointer-based**: the only structure that is copied/moved in
the queue is the lightweight `Event` wrapper; `OrderData/TradeData/PortfolioSnapshot` are not
duplicated for routing.

---

## 1.1 Pool ownership and responsibilities

To avoid confusion, it is useful to make explicit **who owns the pools** and **who is allowed to do
what** with pooled objects.

- **Owner (lifetime, invariants)**  
  - Live: `engines::EventEngine` owns `event_pool_`, `snapshot_pool_`, `order_pool_`,
    `trade_pool_`.
  - Backtest: `backtest::EventEngine` owns the same set of pools.
  - Owners are responsible for:
    - Creating/destroying pools along with the engine.
    - Guaranteeing thread-safe acquire/release semantics.
    - Ensuring all pooled payloads are either processed or released on shutdown (`stop()` drains
      queues and releases payloads).

- **Producers (writers of payload data)**  
  - Examples: `MarketDataClient`, `GatewayClient`, backtest data engine.
  - Allowed operations:
    1. `auto* p = main->acquire_snapshot/order/trade();`
    2. Fill `*p` **only in the producer thread**.
    3. Call `main->put_event(Event(EventType::X, p));`
    4. **Never touch `p` again** (no further reads or writes).
  - Producers **must not** call `release_*` on payloads; ownership is handed off with
    `put_event(...)`.

- **Consumers (readers of payload data)**  
  - Examples:
    - Main worker inside `EventEngine` (live/backtest).
    - Strategy worker (live) when processing strategy events。
  - Allowed operations:
    - Read payloads (`OrderData*`, `TradeData*`, `PortfolioSnapshot*`) directly.
    - After processing, call the appropriate `release_*` (either directly or via
      `release_event_payload`) to return payloads to the pool.
  - Consumers **must not** write back into producer-owned payloads once released; all writes happen
    before `put_event(...)`.

In short:

- **Pools are owned by EventEngines**.
- **Producers acquire + fill + enqueue**, but never release.
- **Consumers dispatch + release**, but never re-fill or reuse payloads on the producer threads.

---

## 2. Live runtime: end-to-end path

### 2.1 MarketData / Gateway → MainEngine

- `MarketDataClient`:

  ```c++
  utilities::PortfolioSnapshot* p = main ? main->acquire_snapshot() : nullptr;
  main_engine_->put_event(Event(EventType::Snapshot, p));
  ```

- `GatewayClient`:

  ```c++
  auto* p = main ? main->acquire_order() : nullptr;
  main_engine_->put_event(Event(EventType::Order, p));

  auto* p = main ? main->acquire_trade() : nullptr;
  main_engine_->put_event(Event(EventType::Trade, p));
  ```

Zero-copy aspect:

- Snapshot/order/trade data is written **directly into pooled objects**; there is no intermediate
  “value” `OrderData` that gets copied into an `Event`.
- `Event` only wraps a pointer and an enum.

### 2.2 MainEngine → EventEngine (queue)

- `engines::MainEngine::put_event` simply forwards to `EventEngine::put_event`:

  ```c++
  void MainEngine::put_event(const utilities::Event& e) { event_engine_->put_event(e); }
  void MainEngine::put_event(utilities::Event&& e)      { event_engine_->put_event(std::move(e)); }
  ```

- Internally, `engines::EventEngine::put_event` delegates to a private `put(const Event&)`:

  ```c++
  Event* p = event_pool_.acquire();
  *p = event;          // copy of lightweight shell
  push_acquired_to_main_ring(p);  // pointer enqueued into ring
  ```

Zero-copy aspect:

- Only the `Event` shell is copied/moved; the payload pointer remains as-is.
- The main ring holds `Event*` (pointers to pooled Events), **not** copies of payload data.

### 2.3 EventEngine main worker → core engines

- Main worker thread dequeues `Event*` and calls `process(Event*)`, which dispatches:

  ```c++
  switch (event->type) {
    case Snapshot: dispatch_snapshot(*event); break;
    case Timer:    dispatch_timer();          break;
    case Order:    dispatch_order(event);     break;
    case Trade:    dispatch_trade(event);     break;
  }
  ```

- `dispatch_order`:

  ```c++
  OrderData* ord = *std::get_if<OrderData*>(&event->data);
  auto* ex  = main->execution_engine();
  auto* pos = main->position_engine();

  std::string strategy_name = ex->get_strategy_name_for_order(ord->orderid);
  ex->process_order_event(strategy_name, *ord);
  if (pos) pos->process_order_event(strategy_name, *ord);
  // OptionStrategyEngine notified via a separate Event into strategy ring (same payload pointer).
  ```

- `dispatch_trade`:

  ```c++
  TradeData* tr = *std::get_if<TradeData*>(&event->data);
  ex->process_trade_event(*tr);
  std::string strategy_name = ex->get_strategy_name_for_order(tr->orderid);
  if (pos) pos->process_trade_event(strategy_name, *tr);
  // Then forwarded to strategy thread same way.
  ```

Zero-copy aspect:

- `ExecutionEngine` / `PositionEngine` / `OptionStrategyEngine` all operate directly on the
  **same `OrderData*`/`TradeData*`** that came from the pool.
- There is no “duplicate `OrderData` for each engine” on this path.

### 2.4 EventEngine main → strategy thread

- When a Timer/Order/Trade is to be delivered to strategies, `EventEngine` allocates a **second
  pooled Event shell** and reuses the same payload pointer:

  ```c++
  Event* p = event_pool_.acquire();
  p->type = EventType::Order;
  p->data = ord;            // same pooled OrderData*
  strategy_ring_.try_push(p);
  ```

- Strategy thread consumes from `strategy_ring_` and calls `process_strategy(const Event&)`, which
  in turn calls `OptionStrategyEngine::process_order_event` / `process_trade_event` with the same
  payload.

Zero-copy aspect:

- There are **two `Event` shells** (one for main queue, one for strategy queue), but **only one
  `OrderData*`/`TradeData*`**.
- When strategy processing is done, the payload is finally released back to the pool.

---

## 3. Backtest runtime: end-to-end path

Backtest follows the same pattern but with a synchronous `EventEngine`:

- `backtest::MainEngine::acquire_*()` forwards to `backtest::EventEngine::acquire_*()`.
- Producers (backtest data engine) call:

  ```c++
  auto* snap = main_engine_->acquire_snapshot();
  // fill snapshot columns
  main_engine_->put_event(Event(EventType::Snapshot, snap));
  ```

- `backtest::EventEngine::put_event`:

  - Acquires an `Event` from its pool.
  - Copies the shell (`*p = event`).
  - Calls `run_dispatch(p)` directly (no background thread).
  - Calls `release_event_payload(p)` to return payloads to pools, then releases the `Event*`.

- Dispatch functions:

  - `dispatch_snapshot` applies the frame to `PortfolioData` via pointer.
  - `dispatch_timer` calls strategies + `PositionEngine::update_metrics`.
  - `dispatch_order` / `dispatch_trade` call `ExecutionEngine` / `PositionEngine` /
    `OptionStrategyEngine` much like live.

Zero-copy aspect:

- Snapshot/Order/Trade payloads are still pooled pointer payloads; only `Event` shells are copied.
- There is no background queue, but the same “pointer payload + pool” contract applies.

---

## 4. Where copies *do* happen (by design)

The core event path is zero-copy with respect to `OrderData/TradeData/Snapshot` payloads, but
certain operations necessarily introduce copies:

- **Network / ZMQ / gRPC boundaries**:

  - Incoming/outgoing messages are serialized/deserialized (`protobuf`, ZMQ message frames).
  - This is unavoidable: wire format ≠ in-memory format.

- **Database persistence**:

  - `engines::MainEngine::save_order_data/save_trade_data` pass `OrderData/TradeData` into
    `DatabaseEngine`, which serializes into SQL/Protobuf representations.

- **Logging**:
  - Internally, log events also use a pool: `LogEngine` owns an `ObjectPool<LogData>`, and
    `MainEngineBase::acquire_log()/release_log()` expose pointer-based logging to producers
    (strategies, core engines).
  - Within the runtime, `LogData*` travels through queues/dispatchers just like other pooled
    payloads (no structural copies of `LogData` itself).
  - At the final sink boundary (formatting for stdout/file/DB/remote), `LogData` is formatted and
    written out, which necessarily creates strings/IO buffers. This is the only place where log
    data is intentionally copied.

- **Backtest columnar data ingestion**:

  - When loading parquet, there is a one-time copy into Arrow/Parquet columnar arrays and internal
    buffers; subsequent per-timestep access is view-based/indexed.

These copies are **off the hot event path**: they either happen once per load, or at system
boundaries (network/DB/logging).

---

## 5. Answering the question: is it still “zero-copy, perfect-forward”?

**Short answer**: Yes, for the internal event pipelines in both live and backtest runtimes, the
current implementation still honors the “pointer payload + perfect-forward” design:

- Producers write into pooled `OrderData/TradeData/Snapshot` objects.
- `Event` only wraps a pointer and is what gets moved/copied in queues.
- Dispatchers (`ExecutionEngine`, `PositionEngine`, `OptionStrategyEngine`, strategies) all see
  the **same pooled payload instance**; no per-engine copies.
- Pools ensure reuse rather than reallocation on the hot path.

Copies are intentionally introduced only:

- At wire boundaries (ZMQ/gRPC/protobuf).
- For logging and DB persistence.
- During one-time backtest data ingestion.

So the **core runtime data path**—from Gateway/MarketData to core engines and strategies—is still
zero-copy in the sense originally described in `low_latency_efforts.md`: heavy data is passed by
pointer, and events are forwarded (and sometimes fanned out) by moving lightweight shells, not by
duplicating payloads.

