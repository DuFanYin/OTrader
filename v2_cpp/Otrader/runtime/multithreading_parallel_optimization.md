# Otrader Runtime 多线程与并行优化说明

本文先从**进程（process）**视角定义运行边界，再下钻到 `MainEngine` 启动流程与线程分工，明确写清楚：

- 哪些线程被创建
- 每个线程负责什么
- 每个队列由谁持有
- 每个队列的生产者 / 消费者分别是谁

---

## 1. 从进程开始：边界与术语

在 live runtime 里，可以把当前程序实例看作**一个进程**。  
这个进程内部至少包含以下两类线程：

- **主控线程（main/control thread）**：进程入口线程，负责初始化与控制 `MainEngine` 生命周期。
- **工作线程（worker threads）**：由各引擎/客户端在进程内创建，负责并行处理事件与 IO。

关键点：**主控线程是进程中的一个线程，不等于进程本身。**

---

## 2. 以 MainEngine 启动为主线

在该进程内，`MainEngine` 初始化时会创建 `EventEngine`，并调用 `event_engine_->start()`。  
`EventEngine::start()` 会创建 3 个内部工作线程：

1. **主消费线程**：`thread_`（执行 `run`）
2. **定时线程**：`timer_thread_`（执行 `run_timer`）
3. **策略线程**：`strategy_thread_`（执行 `run_strategy`）

此外，`MainEngine` 管理的两个客户端在启动后也会各自创建一个订阅线程：

4. **行情订阅线程**：`MarketDataClient::sub_thread_`
5. **网关订阅线程**：`GatewayClient::sub_thread_`

再加上调用侧主控线程（进程入口线程）：

6. **主控线程**：调用 `MainEngine` 对外接口（包括 `put_event`）

结论：在同一个进程内，live 路径中和事件通路直接相关的长期线程可确定为上述 6 类。

---

## 3. 线程职责（逐个说明）

### 2.1 主控线程（Main/control）

- 负责 `MainEngine` 生命周期（构造、启动、关闭）
- 可直接调用 `MainEngine::put_event(...)` 注入事件

### 2.2 行情订阅线程（MarketData SUB）

- 从外部行情进程接收 `Snapshot`
- 申请 `snapshot` 对象池内存并填充
- 调用 `main_engine_->put_event(EventType::Snapshot, p)` 入主队列

### 2.3 网关订阅线程（Gateway SUB）

- 从外部网关进程接收 `Order` / `Trade`
- 申请 `order/trade` 对象池内存并填充
- 调用 `main_engine_->put_event(...)` 入主队列

### 2.4 EventEngine 定时线程（timer_thread_）

- 周期性产生 `Timer` 事件
- 调用 `put(EventType::Timer)` 入主队列

### 2.5 EventEngine 主消费线程（thread_ / run）

- 主事件队列的唯一消费者
- 分发 `Snapshot/Order/Trade/Timer` 到核心引擎
- 需要策略处理时，把事件转发到策略队列（成为策略队列唯一生产者）

### 2.6 EventEngine 策略线程（strategy_thread_ / run_strategy）

- 策略队列的唯一消费者
- 执行策略相关回调（timer/order/trade）

---

## 4. 队列持有关系 + Producer/Consumer（确定性）

### 3.1 主事件队列（MPSC）

- **队列类型**：`utilities::MpscRing<utilities::Event*, 512>`
- **持有者**：`engines::EventEngine`（成员 `queue_ring_`）
- **生产者（4 类）**：
  1. 主控线程（直接 `MainEngine::put_event`）
  2. 行情订阅线程（`MarketDataClient::sub_thread_`）
  3. 网关订阅线程（`GatewayClient::sub_thread_`）
  4. 定时线程（`EventEngine::timer_thread_`）
- **消费者（1 类）**：
  - EventEngine 主消费线程（`thread_` / `run`）

结论：主队列是固定的 **4 Producer -> 1 Consumer**，即典型 `MPSC`。

### 3.2 策略事件队列（SPSC）

- **队列类型**：`utilities::SpscRing<utilities::Event*, 256>`
- **持有者**：`engines::EventEngine`（成员 `strategy_ring_`）
- **生产者（1 类）**：
  - EventEngine 主消费线程（`thread_` / `run`）
- **消费者（1 类）**：
  - EventEngine 策略线程（`strategy_thread_` / `run_strategy`）

结论：策略队列是固定的 **1 Producer -> 1 Consumer**，即典型 `SPSC`。

### 3.3 策略更新队列（MPSC）

- **队列类型**：`utilities::MpscRing<utilities::StrategyUpdateData*, 256>`
- **持有者**：`engines::MainEngine`（`strategy_updates_ring_`）
- **生产者（按当前代码路径）**：
  - `OptionStrategyEngine` 的以下接口会触发 `api_.system.put_strategy_event(...)`，进而进入 `MainEngine::on_strategy_event(...)`：
    - `add_strategy`
    - `init_strategy`
    - `start_strategy`
    - `stop_strategy`
    - `remove_strategy`
  - 这些接口当前由 live gRPC 服务方法调用（`engine_grpc.cpp`）。
- **消费者（按当前代码路径）**：
  - gRPC 流接口 `StreamStrategyUpdates` 循环调用 `MainEngine::pop_strategy_update(...)`。
- **为什么用 MPSC**：
  - 这条链路是“策略管理变更通知流”（新增/初始化/启动/停止/移除策略），用于对外 `StreamStrategyUpdates` 推送。
  - 写入入口由 gRPC 方法触发，服务端请求处理线程默认可并发；当前代码没有对这些入口施加“全局单线程串行化”约束。
  - 因此这里采用 `MPSC` 的依据是：**系统没有单生产者保证**，不是假设“永远只会有一个写入线程”。
  - 只有在未来明确加上单线程执行约束（并长期保证无新增写入源）时，才有条件评估降级为 `SPSC`。

### 3.4 日志流队列（MPSC）

- **队列类型**：`utilities::MpscRing<utilities::LogData*, 1024>`
- **持有者**：`engines::LogEngine`（`stream_ring_`）
- **生产者（当前代码可确认的多来源）**：
  - EventEngine 工作线程路径：`engine_event.cpp` 中调用 `main_engine->put_log_intent(...)`
  - 行情订阅线程路径：`market_data_client.cpp` 出错路径调用 `main_engine_->write_log(...)`
  - 其他引擎路径：通过 `BaseEngine::write_log -> MainEngineBase::put_log_intent -> LogEngine::process_log_intent`
- **消费者（按当前代码路径）**：
  - gRPC 流接口 `StreamLogs` 循环调用 `MainEngine::pop_log_for_stream(...)`。
- **为什么用 MPSC**：
  - 日志写入来自多个线程源头，而流式输出是单消费通道，因此是标准多生产者单消费者模型。

---

## 5. MainEngine 与两个 EventEngine 线程的关系（消除歧义）

- `MainEngine` 是**编排与入口层**：持有组件、管理生命周期、对外暴露接口。
- `EventEngine` 主消费线程是**执行层消费者**：只负责从主队列取事件并分发。
- `EventEngine` 策略线程是**策略执行层消费者**：只负责从策略队列取事件并执行策略回调。

一句话：`MainEngine` 不是“主消费线程”；主消费动作发生在 `EventEngine::run` 线程里。

---

## 6. 并行优化点（现状）

- 热路径使用有界无锁环（`MPSC`/`SPSC`），减少锁竞争
- `Event/Snapshot/Order/Trade/Log` 使用对象池，降低频繁分配释放开销
- 事件只传递轻量壳 + 指针 payload，避免大对象拷贝
- 空队列时用条件变量休眠唤醒，避免长时间空转

---

## 7. Backtest 说明（补充）

- `backtest::EventEngine` 为同步分发模型：`put_event` 后直接 dispatch，不起事件工作线程。
- 多文件回测 `run_backtest_multi(...)` 采用并行 worker（当前固定 4 个 `std::jthread`）。
