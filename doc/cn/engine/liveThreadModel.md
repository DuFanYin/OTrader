# Live 运行时线程模型

本文描述 Otrader Live 下当前的进程/线程划分、各线程职责、队列的持有与 producer/consumer 关系，以及线程间的事件流。

---

## 1. 进程与线程边界

Live 下一个运行实例是**一个进程**。进程内的线程分两类：

- **主控线程（main/control thread）**：进程入口线程，负责初始化与控制 `MainEngine` 生命周期，也可直接调用 `MainEngine::put_event(...)` 注入事件。
- **工作线程（worker threads）**：由各引擎/客户端在进程内创建，负责并行处理事件与 IO。

关键点：**主控线程只是进程中的一个线程，不等于进程本身。**

与事件通路直接相关的长期线程共 **6 类**：EventEngine 的 3 条（主消费、定时、策略）、两个客户端各 1 条 ZMQ 订阅线程、主控线程。此外 gRPC 库使用自身线程池处理 RPC。所有策略逻辑在单一「策略线程」上串行执行，多策略共享该线程。

---

## 2. 线程清单

### 2.1 EventEngine 所属线程（3 条）

由 `EventEngine::start()` 创建；`stop()` 时先置 `active_ = false`、notify 两个 cv，再按 strategy → timer → main 顺序 join，最后 drain 两个 ring 并 release 池内指针。

| 线程 | 入口 | 职责 |
|------|------|------|
| **主消费线程** | `run` | 主事件队列 `queue_ring_`（MPSC）的唯一消费者；空时在 `queue_cv_` 上等待。取到后按类型 dispatch：Snapshot → `dispatch_snapshot`，Timer → `dispatch_timer`，Order → `dispatch_order`，Trade → `dispatch_trade`；需要策略处理时把事件转发到策略队列（成为其唯一生产者）。处理完 `release_event_payload` + `event_pool_.release`。 |
| **定时线程** | `run_timer` | 每 `interval_` 秒调用 `put(Event(Timer))`，把 Timer 事件送入主事件队列。 |
| **策略线程** | `run_strategy` | 策略队列 `strategy_ring_`（SPSC）的唯一消费者；空时在 `strategy_cv_` 上等待。取到后调用 `process_strategy`（按类型调 OptionStrategyEngine 的 on_timer / process_order / process_trade）。处理完 release payload 与 Event。 |

### 2.2 GatewayClient 订阅线程（1 条）

- **创建**：`GatewayClient` 构造时即启动 `sub_thread_`，执行 `run_sub_thread()`。
- **职责**：ZMQ SUB 连接 gateway 的 pub 端，循环收 Order / Trade；反序列化后从 MainEngine 取 `acquire_order()` / `acquire_trade()`，填入并 `put_event(Event(Order/Trade, p))`，向主事件队列推事件。
- **结束**：`close()` 时置 `running_ = false`，join `sub_thread_`。

### 2.3 MarketDataClient 订阅线程（1 条）

- **创建**：`MarketDataClient::start()` 被调用时启动 `sub_thread_`（构造时不启动）。
- **职责**：ZMQ SUB 连接 market data 的 pub 端，循环收 Snapshot；反序列化后 `acquire_snapshot()`，填入并 `put_event(Event(Snapshot, p))`，向主事件队列推事件。
- **结束**：`stop()` 时先发 STOP 再置 `running_ = false`，join `sub_thread_`。

### 2.4 主控线程与 gRPC 入口（entry_system --mode=live）

- **主控线程**：创建 MainEngine、GrpcLiveEngineService，`ServerBuilder::BuildAndStart()` 后调用 `server->Wait()` 阻塞，直到进程退出。可直接调用 `MainEngine::put_event(...)`。
- **gRPC 线程池**：由 gRPC 库管理，用于处理每个 RPC：
  - 同步 RPC（GetStatus、AddStrategy、SendOrder 等）：在池中某线程执行，直接调 MainEngine / OptionStrategyEngine / ExecutionEngine；下单等会通过 `put_intent` 进入 EventEngine，由主消费线程执行。
  - 流式 RPC（StreamLogs、StreamStrategyUpdates）：在 RPC 所在线程循环调用 `pop_log_for_stream(...)` / `pop_strategy_update(...)`，从对应 MPSC ring 取数据写回客户端。

> **MainEngine ≠ 主消费线程**：`MainEngine` 是编排与入口层（持有组件、管理生命周期、对外暴露接口）；主消费动作发生在 `EventEngine::run` 线程里，策略回调发生在策略线程里。

---

## 3. 队列与 Producer/Consumer

四条热路径队列，全部为有界无锁环（`MpscRing` / `SpscRing`），队列空时以条件变量休眠唤醒，避免空转。

### 3.1 主事件队列（MPSC）

- **类型**：`MpscRing<Event*, 512>`，持有者 `EventEngine`（`queue_ring_`）。
- **生产者（4 类）**：① 主控线程（直接 `MainEngine::put_event`）；② 行情订阅线程（Snapshot）；③ 网关订阅线程（Order/Trade）；④ 定时线程（Timer）。
- **消费者（1 类）**：EventEngine 主消费线程（`run`）。
- **同步**：空时主消费线程在 `queue_mutex_` / `queue_cv_` 上等待；任意生产者 `try_push` 成功后 `queue_cv_.notify_one()`。

### 3.2 策略事件队列（SPSC）

- **类型**：`SpscRing<Event*, 256>`，持有者 `EventEngine`（`strategy_ring_`）。
- **生产者（1 类）**：仅主消费线程；在 `dispatch_timer` / `dispatch_order` / `dispatch_trade` 中向 `strategy_ring_` push 事件（每类一份，或按 orderid 路由一份）。
- **消费者（1 类）**：EventEngine 策略线程（`run_strategy`）。
- **同步**：空时策略线程在 `strategy_mutex_` / `strategy_cv_` 上等待；主消费线程 push 后 `strategy_cv_.notify_one()`。

### 3.3 策略更新队列（MPSC）

- **类型**：`MpscRing<StrategyUpdateData*, 256>`，持有者 `MainEngine`（`strategy_updates_ring_`）。
- **生产者**：`OptionStrategyEngine` 的 `add_strategy` / `init_strategy` / `start_strategy` / `stop_strategy` / `remove_strategy` 触发 `api_.system.put_strategy_event(...)` → `MainEngine::on_strategy_event(...)`；当前由 live gRPC 服务方法（`engine_grpc.cpp`）调用。
- **消费者**：gRPC 流接口 `StreamStrategyUpdates` 循环调用 `MainEngine::pop_strategy_update(...)`。
- **为什么用 MPSC**：这是「策略管理变更通知流」，写入入口由 gRPC 方法触发，服务端请求处理线程默认可并发，当前未对这些入口施加全局单线程串行化约束——即**系统没有单生产者保证**。只有在未来明确加上单线程约束并长期保证无新增写入源时，才有条件评估降级为 SPSC。

### 3.4 日志流队列（MPSC）

- **类型**：`MpscRing<LogData*, 1024>`，持有者 `LogEngine`（`stream_ring_`）。
- **生产者（多来源）**：EventEngine 工作线程路径（`engine_event.cpp` 调 `put_log_intent`）、行情订阅线程出错路径（`market_data_client.cpp` 调 `write_log`）、其他引擎（`BaseEngine::write_log` → `MainEngineBase::put_log_intent` → `LogEngine::process_log_intent`）。
- **消费者**：gRPC 流接口 `StreamLogs` 循环调用 `MainEngine::pop_log_for_stream(...)`。
- **为什么用 MPSC**：日志写入来自多个线程源头，而流式输出是单消费通道，标准多生产者单消费者模型。

### 3.5 事件流小结

```
[Timer thread]     --> put(Timer)            -->\
[GatewayClient]    --> put_event(Order/Trade) --> queue_ring_ (MPSC) --> main worker --> dispatch_*
[MarketDataClient] --> put_event(Snapshot)   -->/  [主控线程亦可直接 put_event]  |
                                                                                  | dispatch_timer / dispatch_order / dispatch_trade
                                                                                  v
                                                          strategy_ring_ (SPSC) --> strategy thread --> process_strategy
```

---

## 4. 其他与线程相关的数据流

- **Intent（下单/撤单/日志）**：由主消费线程在 `dispatch_timer` 内（PositionEngine、HedgeEngine）或由策略线程在策略回调里通过 RuntimeAPI 调用，最终进入 `EventEngine::put_intent`；`put_intent` 在**调用者线程**中同步执行（如 send_order 在主消费或策略线程执行），不经过事件队列。
- **Log 流 / StrategyUpdate 流**：见 §3.3、§3.4——均为引擎侧写入 MPSC ring、gRPC 流线程消费。

---

## 5. 生命周期与关闭顺序

- **MainEngine 构造**：创建 EventEngine、LogEngine、ExecutionEngine、OptionStrategyEngine、PositionEngine、HedgeEngine、PortfolioStructure、GatewayClient、MarketDataClient 等。
- **启动**：`connect()` 启动 EventEngine（`start()`）并连接 Gateway（GatewayClient 的 sub_thread_ 在构造时已启动）；`start_market_data_update()` 启动 MarketDataClient 的 sub_thread_。
- **断开与关闭**：`disconnect()` 断开 gateway/market data（并 join 订阅线程）；`close()` 调 EventEngine `stop()`，join 三条线程并 drain 两个 ring。
- **gRPC 进程退出**：先 `main_engine.disconnect()`、`main_engine.close()`，再析构 server——先停客户端与 EventEngine，再收口 gRPC。

---

## 6. 并行优化点（现状）

- 热路径使用有界无锁环（MPSC/SPSC），减少锁竞争。
- `Event/Snapshot/Order/Trade/Log` 使用对象池，降低频繁分配释放开销。
- 事件只传递轻量壳 + 指针 payload，避免大对象拷贝（详见 [zeroCopyDataPath](../../en/engine/zeroCopyDataPath.md)）。
- 空队列时条件变量休眠唤醒，避免空转。

---

## 7. Backtest 说明

- `backtest::EventEngine` 为同步分发模型：`put_event` 后直接 dispatch，不起事件工作线程。
- 多文件回测 `run_backtest_multi(...)` 采用并行 worker（当前 `num_engines = 4` 个 `std::jthread`，共享 stop token，按 file_index 顺序归并结果）。

---

## 8. 线程小结表

| 线程来源 | 数量 | 消费 / 生产 |
|----------|------|------------|
| 主控线程 | 1 | 生产 queue_ring_（直接 put_event）；管理生命周期 |
| EventEngine 主消费 | 1 | 消费 queue_ring_；生产 strategy_ring_（Timer/Order/Trade） |
| EventEngine 定时 | 1 | 生产 queue_ring_（Timer） |
| EventEngine 策略 | 1 | 消费 strategy_ring_；调策略 on_timer/process_order/process_trade |
| GatewayClient | 1 | 生产 queue_ring_（Order, Trade） |
| MarketDataClient | 1 | 生产 queue_ring_（Snapshot） |
| gRPC 池 | 库默认 | 调 MainEngine；StreamLogs/StreamStrategyUpdates 消费 log/strategy_update ring |

所有策略共享一条 strategy 线程；若需每策略一线程，见 [perStrategyThread](./perStrategyThread.md)。
```
