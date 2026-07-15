# Live 运行时线程模型

本文描述 Otrader Live 下当前的线程划分、各自职责及线程间通信方式。

---

## 1. 总览

Live 进程内与事件处理相关的工作分布在以下几类线程上：

- **EventEngine 内**：主事件工作线程、定时线程、策略线程（共 3 条）。
- **外部客户端**：GatewayClient、MarketDataClient 各一条 ZMQ 订阅线程（连接/启动后存在）。
- **gRPC 入口**（`entry_live_grpc`）：主线程阻塞在 `server->Wait()`；gRPC 库使用自身线程池处理 RPC，RPC 内直接调用 MainEngine。

无额外业务线程；策略逻辑全部在「策略线程」上串行执行，多策略共享该线程。

---

## 2. 线程清单

### 2.1 EventEngine 所属线程（3 条）

由 `EventEngine::start()` 创建，`stop()` 时先置 `active_ = false`、notify 两个 cv，再按 strategy → timer → main 顺序 join，最后 drain 两个 ring 并 release 池内指针。

| 线程 | 入口 | 职责 |
|------|------|------|
| **Main worker** | `run(st)` | 从主事件队列 `queue_ring_`（MPSC）取事件；空时在 `queue_cv_` 上等待。取到后按类型 dispatch：Snapshot → `dispatch_snapshot`，Timer → `dispatch_timer`，Order → `dispatch_order`，Trade → `dispatch_trade`。处理完后 `release_event_payload` + `event_pool_.release`。 |
| **Timer** | `run_timer(st)` | 每 `interval_` 秒往 EventEngine 推一条 `Event(Timer)`（调用 `put(Event(Timer))`），即把 Timer 事件送入主事件队列。 |
| **Strategy** | `run_strategy(st)` | 从策略事件队列 `strategy_ring_`（SPSC）取事件；空时在 `strategy_cv_` 上等待。取到后调用 `process_strategy`（按事件类型调 OptionStrategyEngine 的 on_timer / process_order / process_trade）。处理完后 release payload 与 Event。 |

### 2.2 GatewayClient 订阅线程（1 条）

- **创建**：`GatewayClient` 构造时即启动 `sub_thread_`，执行 `run_sub_thread()`。
- **职责**：ZMQ SUB 连接 gateway 的 pub 端，循环收 Order / Trade 消息；反序列化后从 MainEngine 取 `acquire_order()` / `acquire_trade()`，填入并 `put_event(Event(Order, p))` 或 `put_event(Event(Trade, p))`，即向 EventEngine 的**主事件队列**推事件。
- **结束**：`close()` 时置 `running_ = false`，join `sub_thread_`。

### 2.3 MarketDataClient 订阅线程（1 条）

- **创建**：`MarketDataClient::start()` 被调用时启动 `sub_thread_`，执行 `run_sub_thread()`（构造时不启动）。
- **职责**：ZMQ SUB 连接 market data 的 pub 端，循环收 Snapshot 消息；反序列化后 `acquire_snapshot()`，填入并 `put_event(Event(Snapshot, p))`，向 EventEngine 的**主事件队列**推事件。
- **结束**：`stop()` 时先发 STOP 再置 `running_ = false`，join `sub_thread_`。

### 2.4 gRPC 入口（entry_live_grpc）

- **主线程**：创建 MainEngine、GrpcLiveEngineService，`ServerBuilder::BuildAndStart()` 后调用 `server->Wait()` 阻塞，直到进程退出。
- **gRPC 线程池**：由 gRPC 库管理，用于处理每个 RPC。例如：
  - 同步 RPC（GetStatus、AddStrategy、SendOrder 等）：在池中某线程执行，直接调 MainEngine / OptionStrategyEngine / ExecutionEngine，其中下单等会通过 `put_intent` 进入 EventEngine，由 main worker 执行。
  - 流式 RPC（StreamLogs、StreamStrategyUpdates）：该 RPC 所在线程在循环里调用 `main_engine_->pop_log_for_stream(...)` 或 `main_engine_->pop_strategy_update(...)`，从 LogEngine / MainEngine 的 MPSC ring 取数据并写回客户端。

---

## 3. 事件与队列

### 3.1 主事件队列（queue_ring_）

- **类型**：`MpscRing<Event*, 512>`，多生产者、单消费者。
- **生产者**：Timer 线程（Timer 事件）、GatewayClient 订阅线程（Order/Trade）、MarketDataClient 订阅线程（Snapshot）；若将来有其他地方调用 `MainEngine::put_event`，也会成为生产者。
- **消费者**：EventEngine 的 main worker（`run()`）。
- **同步**：队列空时 main worker 在 `queue_mutex_` / `queue_cv_` 上等待；任意生产者 `try_push` 成功后 `queue_cv_.notify_one()`。

### 3.2 策略事件队列（strategy_ring_）

- **类型**：`SpscRing<Event*, 256>`，单生产者、单消费者。
- **生产者**：仅 main worker；在 `dispatch_timer` / `dispatch_order` / `dispatch_trade` 中向 `strategy_ring_` push Timer/Order/Trade 事件（每类事件一份或按 orderid 路由一份）。
- **消费者**：EventEngine 的 strategy 线程（`run_strategy()`）。
- **同步**：队列空时 strategy 线程在 `strategy_mutex_` / `strategy_cv_` 上等待；main worker push 后 `strategy_cv_.notify_one()`。

### 3.3 事件流小结

```
[Timer thread]     --> put(Timer)     -->\
[GatewayClient]    --> put_event(Order/Trade) --> queue_ring_ (MPSC) --> main worker --> dispatch_* 
[MarketDataClient] --> put_event(Snapshot)   -->/                          |
                                                                           | dispatch_timer / dispatch_order / dispatch_trade
                                                                           v
                                                              strategy_ring_ (SPSC) --> strategy thread --> process_strategy
```

---

## 4. 其他与线程相关的数据流

- **Intent（下单/撤单/日志）**：由 main worker 在 `dispatch_timer` 内（PositionEngine、HedgeEngine）或由 strategy 线程在策略回调里通过 RuntimeAPI 调用，最终进入 `EventEngine::put_intent`；`put_intent` 在**调用者线程**中同步执行（如 send_order 在 main 或 strategy 线程执行），不经过事件队列。
- **Log 流**：各引擎通过 `put_log_intent` → LogEngine `process_log_intent`，内部可 push 到 `stream_ring_`（MPSC）；gRPC 的 StreamLogs 在 gRPC 线程中循环 `pop_log_for_stream`，消费该 ring。
- **StrategyUpdate 流**：策略侧通过 `put_strategy_event` 等写入 `strategy_updates_ring_`（MPSC）；gRPC 的 StreamStrategyUpdates 在 gRPC 线程中循环 `pop_strategy_update` 消费。

---

## 5. 生命周期与顺序

- **MainEngine**：构造时创建 EventEngine、LogEngine、ExecutionEngine、OptionStrategyEngine、PositionEngine、HedgeEngine、PortfolioStructure、GatewayClient、MarketDataClient 等；`connect()` 时启动 EventEngine（`start()`）并连接 Gateway（GatewayClient 的 sub_thread_ 在构造时已启动）；`start_market_data_update()` 启动 MarketDataClient 的 sub_thread_。
- **断开与关闭**：`disconnect()` 断开 gateway/market data（并 join 订阅线程）；`close()` 调 EventEngine `stop()`，join 三条线程并 drain 两个 ring。
- **gRPC 进程**：退出时先 `main_engine.disconnect()`、`main_engine.close()`，再析构 server，保证先停客户端与 EventEngine，再收口 gRPC。

---

## 6. 小结表

| 线程来源 | 数量 | 消费/生产 |
|----------|------|------------|
| EventEngine main worker | 1 | 消费 queue_ring_；生产 strategy_ring_（Timer/Order/Trade） |
| EventEngine timer | 1 | 生产 queue_ring_（Timer） |
| EventEngine strategy | 1 | 消费 strategy_ring_；调策略 on_timer/process_order/process_trade |
| GatewayClient | 1 | 生产 queue_ring_（Order, Trade） |
| MarketDataClient | 1 | 生产 queue_ring_（Snapshot） |
| gRPC 池 | 库默认 | 调 MainEngine；StreamLogs/StreamStrategyUpdates 消费 log/strategy_update ring |

所有策略共享一条 strategy 线程；若需每策略一线程，见 `docs/per_strategy_thread.md`。
