# Live Runtime Thread Model

The current thread layout of the Otrader Live runtime: which threads exist, what each does, how the queues are owned, and how events flow. (Chinese, more detailed: [../../cn/engine/liveThreadModel.md](../../cn/engine/liveThreadModel.md).)

---

## 1. Process and thread boundary

A running Live instance is **one process**. Its threads fall into two groups:

- **Main/control thread** — the process entry thread. Initializes and controls the `MainEngine` lifecycle, and may also inject events directly via `MainEngine::put_event(...)`.
- **Worker threads** — created by engines/clients inside the process to handle events and IO in parallel.

Six long-lived threads sit on the event path: the EventEngine's 3 (main consumer, timer, strategy), one ZMQ subscription thread per client (gateway, market data), and the main/control thread. gRPC additionally uses its own library-managed thread pool for RPCs. All strategy logic runs serially on the single shared strategy thread.

> `MainEngine` is the orchestration/entry layer, **not** the consuming thread. Main consumption happens on the `EventEngine::run` thread; strategy callbacks run on the strategy thread.

---

## 2. Thread list

### 2.1 EventEngine threads (3)

Created by `EventEngine::start()`. On `stop()`: set `active_ = false`, notify both CVs, join in strategy → timer → main order, then drain both rings and release pooled pointers.

| Thread | Entry | Responsibility |
|--------|-------|----------------|
| **Main consumer** | `run` | Sole consumer of the main queue `queue_ring_` (MPSC); waits on `queue_cv_` when empty. Dispatches by type: Snapshot → `dispatch_snapshot`, Timer → `dispatch_timer`, Order → `dispatch_order`, Trade → `dispatch_trade`. When strategy handling is needed, forwards the event to the strategy queue (becoming its sole producer). Releases payload + event afterward. |
| **Timer** | `run_timer` | Every `interval_` seconds calls `put(Event(Timer))`, pushing a Timer event onto the main queue. |
| **Strategy** | `run_strategy` | Sole consumer of the strategy queue `strategy_ring_` (SPSC); waits on `strategy_cv_` when empty. Runs `process_strategy` (OptionStrategyEngine on_timer / process_order / process_trade). Releases payload + event afterward. |

### 2.2 Client subscription threads (2)

- **GatewayClient** — started in the constructor (`run_sub_thread()`). ZMQ SUB to the gateway's PUB; receives Order/Trade, acquires pooled objects from MainEngine, and `put_event(Order/Trade)` onto the main queue. Joined on `close()`.
- **MarketDataClient** — started on `start()` (not in the constructor). ZMQ SUB to the market-data PUB; receives Snapshot, acquires a pooled snapshot, and `put_event(Snapshot)` onto the main queue. Joined on `stop()` (sends STOP first).

### 2.3 Main/control thread and gRPC (`entry_system --mode=live`)

- **Main/control thread**: builds MainEngine and the gRPC service; after `ServerBuilder::BuildAndStart()` it blocks on `server->Wait()` until process exit. May call `MainEngine::put_event(...)` directly.
- **gRPC thread pool** (library-managed):
  - Sync RPCs (GetStatus, AddStrategy, SendOrder, …) run on a pool thread and call MainEngine / OptionStrategyEngine / ExecutionEngine directly; order submission goes through `put_intent` into the EventEngine and executes on the main consumer thread.
  - Streaming RPCs (StreamLogs, StreamStrategyUpdates) loop on `pop_log_for_stream(...)` / `pop_strategy_update(...)`, consuming the corresponding MPSC rings and writing back to the client.

---

## 3. Queues and producer/consumer

Four hot-path queues, all bounded lock-free rings; threads sleep/wake on a condition variable when a queue is empty.

| Queue | Type | Owner | Producers | Consumer |
|-------|------|-------|-----------|----------|
| **Main event** | `MpscRing<Event*, 512>` | EventEngine (`queue_ring_`) | 4: main/control (direct `put_event`), market-data sub (Snapshot), gateway sub (Order/Trade), timer (Timer) | Main consumer (`run`) |
| **Strategy event** | `SpscRing<Event*, 256>` | EventEngine (`strategy_ring_`) | 1: main consumer (in `dispatch_*`) | Strategy thread (`run_strategy`) |
| **Strategy update** | `MpscRing<StrategyUpdateData*, 256>` | MainEngine (`strategy_updates_ring_`) | OptionStrategyEngine add/init/start/stop/remove_strategy → `put_strategy_event` (currently via gRPC service methods) | gRPC `StreamStrategyUpdates` → `pop_strategy_update` |
| **Log stream** | `MpscRing<LogData*, 1024>` | LogEngine (`stream_ring_`) | Multiple: EventEngine paths, client error paths, any engine via `BaseEngine::write_log` | gRPC `StreamLogs` → `pop_log_for_stream` |

**Why MPSC for the strategy-update and log streams**: both have no single-producer guarantee — strategy-management calls arrive on concurrent gRPC request threads, and logs originate from many threads — while the streaming output is a single consumer. MPSC could only be revisited as SPSC if a single-writer constraint were later enforced and guaranteed.

### Event flow

```
[Timer thread]     --> put(Timer)             -->\
[GatewayClient]    --> put_event(Order/Trade)  --> queue_ring_ (MPSC) --> main consumer --> dispatch_*
[MarketDataClient] --> put_event(Snapshot)    -->/  [main/control thread may put_event too]  |
                                                                                            | dispatch_timer / order / trade
                                                                                            v
                                                        strategy_ring_ (SPSC) --> strategy thread --> process_strategy
```

---

## 4. Other thread-related flows

- **Intents (order/cancel/log)** are produced by the main consumer (PositionEngine/HedgeEngine in `dispatch_timer`) or by the strategy thread (strategy callbacks) via RuntimeAPI, reaching `EventEngine::put_intent`, which runs **synchronously on the caller thread** — it does not go through the event queue.
- **Log / strategy-update streams**: engine-side writes into the MPSC rings above; gRPC streaming threads consume them.

---

## 5. Lifecycle and shutdown order

- **Construction**: MainEngine builds EventEngine, LogEngine, ExecutionEngine, OptionStrategyEngine, PositionEngine, HedgeEngine, PortfolioStructure, GatewayClient, MarketDataClient.
- **Startup**: `connect()` starts the EventEngine and connects the gateway (GatewayClient's sub thread is already running from its constructor); `start_market_data_update()` starts MarketDataClient's sub thread.
- **Shutdown**: `disconnect()` disconnects gateway/market data (joins sub threads); `close()` calls EventEngine `stop()` (joins the 3 threads, drains both rings). The gRPC process exits by `disconnect()` → `close()` → destroy server — clients and EventEngine first, gRPC last.

---

## 6. Backtest

The backtest `EventEngine` dispatches synchronously (`put_event` dispatches inline, no worker thread). Multi-file backtest `run_backtest_multi(...)` runs parallel workers (currently `num_engines = 4` `std::jthread`s sharing a stop token, results merged in file order).

---

The single shared strategy thread is a planned upgrade point — see [../../cn/engine/perStrategyThread.md](../../cn/engine/perStrategyThread.md) (Chinese) for the per-strategy-thread plan. Low-latency rationale: [lowLatencyEfforts](./lowLatencyEfforts.md); zero-copy data path: [zeroCopyDataPath](./zeroCopyDataPath.md).
```
