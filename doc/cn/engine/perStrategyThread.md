# 每策略独立线程升级方案

目标：Live 下为每个策略实例分配独立线程，策略逻辑（on_timer / on_order / on_trade）在各自线程中执行，互不阻塞。

---

## 1. 当前模型（单策略线程）

当前所有策略共享一条 `strategy_thread_`（`run_strategy()`），从单一 SPSC `strategy_ring_` 顺序消费 Timer/Order/Trade，`on_timer` 遍历所有策略依次执行——详见 [liveThreadModel](./liveThreadModel.md) §2.1、§3.2。

结论：多策略共享一条线程，**一个策略卡住会拖慢其他策略**——这正是本升级方案要解决的问题。

---

## 2. 目标模型（每策略一线程）

- 每个策略实例对应：**一条专用线程** + **一个该策略专属的 SPSC 队列**。
- Timer：main worker 为**每个已注册策略**推一条 Timer 事件到该策略的队列（或广播到各策略队列）。
- Order/Trade：main worker 按 orderid 解析出 strategy_name，将事件只推入**该策略的队列**。
- 策略线程只消费自己的队列，只执行自己的 `on_timer` / `on_order` / `on_trade`。

---

## 3. 需要做的升级

### 3.1 事件路由与队列结构

| 当前 | 目标 |
|------|------|
| 一个 `strategy_ring_`，所有策略事件混在一起 | 按策略名分队列：`map<string, SpscRing<Event*, N>>` 或 `map<string, unique_ptr<SpscRing>>` |
| main worker 推事件不区分策略 | Timer：遍历 `get_strategy_names()`，每个策略推一条 Timer 到其队列；Order/Trade：`get_strategy_name_for_order(orderid)` 得到 strategy_name，只推入该策略队列 |

- 若策略很多，Timer 需推 N 条（每策略一条）；或保留“广播 Timer”由某分发线程再按策略转发，复杂度更高，一般直接 N 条即可。
- 队列容量：每策略一个 SPSC ring（如 256），或按策略名动态创建/销毁。

### 3.2 策略线程生命周期

| 当前 | 目标 |
|------|------|
| 启动时一条 `strategy_thread_`，固定跑 `run_strategy()` | `add_strategy(name, ...)` 时：创建该策略的队列（若尚未存在），创建并启动 `strategy_thread_[name]`，跑 `run_strategy_for(name)` |
| 无 per-strategy 线程 | `remove_strategy(name)` 时：置关闭标志，notify 该策略线程，join；排空该策略队列并 release 事件 payload；删除队列与线程句柄 |

- 线程句柄存储：`map<string, jthread>` 或 `map<string, unique_ptr<jthread>>`，与队列同生命周期。

### 3.3 OptionStrategyEngine 与共享状态线程安全

当前策略回调里会调用：

- `api_.execution.send_order` / `get_order` / `get_strategy_name_for_order` / `get_strategy_active_orders` 等
- `api_.portfolio.get_portfolio` / `get_contract` 等
- `api_.system.write_log` / `get_holding` 等

多策略多线程后，**ExecutionEngine**（orders_/trades_/strategy_active_orders_）、**PortfolioStructure** / **PortfolioData**、**OptionStrategyEngine** 自身（strategies_、get_strategy 等）会被多线程并发读/写，需要：

- 对共享结构加锁（如 ExecutionEngine 的 map 操作、Portfolio 的读/写），或
- 将“按 strategy_name 划分”的读写尽量限制到 per-strategy 数据，减少跨策略共享；共享部分用 mutex 或 rwlock 保护。

具体建议：

- **ExecutionEngine**：已有或引入 mutex，保护 `orders_`、`trades_`、`strategy_active_orders_`、`orderid_strategy_name_` 的读写。
- **PortfolioStructure / PortfolioData**：若存在多线程写（例如 apply_frame 与策略读 portfolio 并发），需要读/写锁或拷贝+替换策略；若仅 main worker 写、策略只读，可考虑读锁 + 单写。
- **OptionStrategyEngine**：`strategies_`、`get_strategy`、`get_strategy_holding` 等被多线程访问时加锁或保证“只在 add/remove 时写、其余读”并用 rwlock。

### 3.4 EventEngine 改动要点

- **队列与线程**：由单一 `strategy_ring_` + `strategy_thread_` 改为 `unordered_map<string, StrategyThreadContext>`，其中每个 context 含：`SpscRing<Event*, N>`、`jthread`、`mutex`+`cv`（用于 wait/notify）、关闭标志。
- **dispatch_timer**（main worker）：不再向一个 ring 推一条 Timer；改为对 `option_strategy_engine()->get_strategy_names()` 遍历，对每个 name 取该策略的 ring，push Timer 事件并 notify 该策略的 cv。
- **dispatch_order / dispatch_trade**（main worker）：先 `get_strategy_name_for_order(orderid)`，若为空可 fallback 或丢；再向**该 strategy_name 的 ring** push Order/Trade 事件，并 notify 该策略 cv。
- **run_strategy**：改为 `run_strategy_for(strategy_name)`，只从该 name 的 ring 里 pop，只调 `get_strategy(name)->on_timer()/process_order/process_trade`（或直接在该线程内根据 event.type 调对应策略方法）。
- **stop()**：遍历所有 strategy context，置关闭、notify、join；再逐个 drain 各策略 ring 并 release payload；最后清理 map。

### 3.5 与 dispatch_timer 的衔接（HedgeEngine / PositionEngine）

当前 main worker 在 `process()` 里对 Snapshot/Order/Trade 做 dispatch，对 Timer 会调 `dispatch_timer()`（产生 orders/cancels/logs），再向 strategy_ring_ 推一条 Timer。改为每策略一线程后：

- **dispatch_timer** 仍由 main worker 执行（调 PositionEngine、HedgeEngine，得到 orders/cancels/logs），逻辑可不变。
- 之后不再向“单一 strategy_ring_”推一条 Timer，而是向**每个策略的 ring** 各推一条 Timer 事件；各策略线程各自 pop 后只执行自己的 `on_timer()`。

这样 Timer 的“全局逻辑”（position/hedge）仍在 main worker，策略的 `on_timer()` 在各自线程。

### 3.6 Backtest

- Backtest 保持单线程、同步 `put_event` → `run_dispatch`，不引入 per-strategy 线程。
- 仅 Live EventEngine 做上述“每策略一线程 + 每策略一队列”改造。

---

## 4. 实现顺序建议

1. **EventEngine**：引入 `map<string, StrategyContext>`（ring + thread + cv + closed），先只支持“单策略名”的调度（与当前单 strategy_thread_ 行为等价），保证 run_strategy_for 只处理该策略的事件。
2. **事件路由**：main worker 在 dispatch 后，Timer 按 `get_strategy_names()` 推多份；Order/Trade 按 orderid 解析 strategy_name 推入对应 ring；notify 对应 strategy 的 cv。
3. **add_strategy / remove_strategy**：在 OptionStrategyEngine 或 EventEngine 层创建/销毁 StrategyContext（队列+线程），与策略注册/注销同步。
4. **共享状态**：给 ExecutionEngine、Portfolio 相关、OptionStrategyEngine 的 strategies_ 访问路径加锁或 rwlock，并做并发测试。
5. **stop() / drain**：按策略遍历 join + drain，避免漏 release 或 UAF。

---

## 5. 风险与注意点

- **线程数**：策略数量即线程数，策略很多时需考虑上限或线程池（本方案为“每策略一线程”，不采用线程池）。
- **顺序**：同一策略内事件仍按队列顺序执行；不同策略之间无顺序保证。
- **跨策略共享**：若策略 A 的回调里访问了“策略 B 的 holding”或全局状态，需要明确锁顺序，避免死锁。
- **测试**：重点覆盖多策略并发、add/remove 与事件并发、stop 时无泄漏无 UAF。

---

## 6. 具体改动清单（文件 / 类 / 函数）

按实现顺序列出需改动的代码位置，便于按图施工。

### 步骤 1：EventEngine 引入 per-strategy context

| 位置 | 改动 |
|------|------|
| **`runtime/live/engine_event.hpp`** | 定义 `StrategyContext`（或匿名 struct）：含 `SpscRing<Event*, 256>`、`std::mutex`、`std::condition_variable_any`、`std::jthread`、`std::atomic<bool> closed`。将 `strategy_ring_`、`strategy_mutex_`、`strategy_cv_`、`strategy_thread_` 替换为 `std::unordered_map<std::string, StrategyContext> strategy_contexts_`（或 `map<string, unique_ptr<StrategyContext>>`）。需持 MainEngine 或 EventEngine 指针以在 run 中取 option_strategy_engine。 |
| **`runtime/live/engine_event.cpp`** | `run_strategy()` 改为 `run_strategy_for(const std::string& strategy_name)`：只从 `strategy_contexts_[strategy_name]` 的 ring 取事件，只调 `get_strategy(strategy_name)` 的 on_timer/process_order/process_trade。`start()` 中不再启动单一 strategy_thread_，改为在 add_strategy 时按 name 启动；或暂时保留“单 strategy 时兼容”：若 `strategy_contexts_.size()==1` 则启动一条线程跑该唯一 context。 |

### 步骤 2：事件路由（Timer / Order / Trade 按策略入队）

| 位置 | 改动 |
|------|------|
| **`runtime/live/engine_event.cpp`** `dispatch_timer()` | 末尾当前：`event_pool_.acquire()` → 填 Timer → `strategy_ring_.try_push(p)` → `strategy_cv_.notify_one()`。改为：对 `se->get_strategy_names()` 遍历，对每个 `strategy_name` 从 `strategy_contexts_[strategy_name]` 取 ring，acquire 一个 Event 填 Timer，try_push 到该 ring，notify 该 context 的 cv；若某策略 ring 满则 release 并跳过或重试。 |
| **`runtime/live/engine_event.cpp`** `dispatch_order(Event* event)` | 在取得 `ord` 后，先 `ex->get_strategy_name_for_order(ord->orderid)` 得到 `strategy_name`；若空可 fallback 到 `se->get_strategy_names()` 首个或丢。再只向 `strategy_contexts_[strategy_name].ring` push 该 Order 事件，并 notify 该 context 的 cv。 |
| **`runtime/live/engine_event.cpp`** `dispatch_trade(Event* event)` | 同上，用 `tr->orderid` 查 `get_strategy_name_for_order`，只向对应该 strategy_name 的 context 的 ring push，并 notify 其 cv。 |

### 步骤 3：add_strategy / remove_strategy 与 context 生命周期

| 位置 | 改动 |
|------|------|
| **`core/engine_option_strategy.cpp`** `add_strategy()` | 在 `strategies_[strategy_name] = std::move(ptr)` 之后，通知 EventEngine 为该 strategy_name 创建 StrategyContext（ring + cv + closed），并启动 `run_strategy_for(strategy_name)` 线程。需 EventEngine 暴露接口，例如 `event_engine()->ensure_strategy_thread(strategy_name)` / `create_strategy_context(strategy_name)`。 |
| **`core/engine_option_strategy.cpp`** `remove_strategy()` | 在 `strategies_.erase(it)` 之前，通知 EventEngine 关闭并 join 该 strategy_name 的线程、drain 其 ring 并 release payload、删除 context。例如 `event_engine()->remove_strategy_thread(strategy_name)`。 |
| **`runtime/live/engine_event.hpp`** | 声明 `void ensure_strategy_context(const std::string& strategy_name);`、`void remove_strategy_context(const std::string& strategy_name);`（或等价命名）。实现中：ensure 时若 map 中无 name 则插入 StrategyContext 并启动 jthread；remove 时置 closed、notify、join、drain ring、erase。 |
| **`runtime/live/engine_main.cpp`** 或 **engine_option_strategy 构造/API** | OptionStrategyEngine 需能访问 EventEngine（例如 MainEngine 传入 callback 或 EventEngine*），以便在 add/remove 时调 ensure/remove_strategy_context。若当前没有直接引用，可在 MainEngine 注册时注入 `std::function<void(const std::string&)> on_strategy_added` / `on_strategy_removed`，由 Main 转调 event_engine 的 ensure/remove。 |

### 步骤 4：共享状态加锁

| 位置 | 改动 |
|------|------|
| **`core/engine_execution.hpp`** | 增加 `mutable std::mutex mutex_`（或 rwlock）。 |
| **`core/engine_execution.cpp`** | 所有对 `orders_`、`trades_`、`orderid_strategy_name_`、`strategy_active_orders_`、`all_active_order_ids_` 的读/写（如 `store_order`、`store_trade`、`get_order`、`get_strategy_name_for_order`、`remove_order_tracking`、`clear` 等）包在 `std::lock_guard`（或 shared_mutex 读锁）内。 |
| **`core/engine_option_strategy.hpp`** | 增加 `mutable std::shared_mutex strategies_mutex_`（或 mutex）。 |
| **`core/engine_option_strategy.cpp`** | `get_strategy(name)`、`get_strategy()`、`get_strategy_holding`、`get_strategy_names`、`on_timer` 中遍历 strategies_、`add_strategy`、`remove_strategy` 等读/写 `strategies_` 的地方加读锁或写锁。 |
| **`core/portfolio_structure.hpp`** / **portfolio 相关** | 若 PortfolioStructure / PortfolioData 在 main worker 写（apply_frame）的同时被多策略线程读，需在 get_portfolio、apply_frame、或各 portfolio 的读接口上加 rwlock；具体视现有实现而定。 |

### 步骤 5：stop() 与 drain

| 位置 | 改动 |
|------|------|
| **`runtime/live/engine_event.cpp`** `stop()` | 在 join main worker / timer 之后，遍历 `strategy_contexts_`：对每个 context 置 closed、notify_all 其 cv、join 其 jthread；再对该 context 的 ring 做 while (try_pop(e)) release_event(e)；最后 `strategy_contexts_.clear()` 或逐个 erase。 |
| **`runtime/live/engine_event.hpp`** | 若 StrategyContext 的 jthread 在 join 后析构，ring 与 mutex/cv 的析构顺序需保证：先 join，再 drain ring，再析构 ring 与 cv。 |

### 涉及文件汇总

- **Live EventEngine**：`runtime/live/engine_event.hpp`，`runtime/live/engine_event.cpp`（dispatch_timer、dispatch_order、dispatch_trade、run_strategy_for、start、stop、ensure_strategy_context、remove_strategy_context）。
- **OptionStrategyEngine**：`core/engine_option_strategy.hpp`（strategies_ 的锁），`core/engine_option_strategy.cpp`（add_strategy、remove_strategy 内调 EventEngine 的 ensure/remove；get_strategy、on_timer 等加读锁）。
- **ExecutionEngine**：`core/engine_execution.hpp`（mutex 成员），`core/engine_execution.cpp`（各 map 访问加锁）。
- **MainEngine**：`runtime/live/engine_main.cpp` 或 `.hpp`（若需在 OptionStrategyEngine 与 EventEngine 之间注入 add/remove 回调）。
- **Backtest**：不改；`runtime/backtest/engine_event.*` 保持单线程同步。


---

## 7. 与 v1 Python 的对应关系

**结论**：与 v1_python 一致——**不是每个策略独立线程**；所有策略的 `on_timer` / `on_order` / `on_trade` 都在**同一条线程**里顺序执行。

| 维度 | v1 Python（engine_event / engine_strategy） | Live C++（当前） |
|------|---------------------------------------------|------------------|
| 事件队列 | 1 个 Queue，Timer/Order/Trade/Snapshot 都进同一队列 | 2 个队列：主队列 `queue_ring_`（Snapshot/Order/Trade/Timer 入队）、策略队列 `strategy_ring_`（仅 Timer/Order/Trade，由 main worker 推入） |
| 事件消费线程 | 1 条 `_thread` 跑 `_run()`：从队列取事件 → `_process(event)` → 按类型调已注册 handler（含 process_timer_event / process_order_event / process_trade_event），在这些 handler 里再按 strategy_name 调各策略的 on_timer/on_order/on_trade | **Main worker**：只消费主队列，做 dispatch_snapshot、dispatch_timer（Position/Hedge + 向 strategy_ring_ 推 Timer）、dispatch_order/dispatch_trade（向 strategy_ring_ 推 Order/Trade）。**Strategy 线程**：消费 strategy_ring_，调 `process_strategy` → OptionStrategyEngine 的 on_timer/process_order/process_trade（即所有策略回调都在此线程） |
| Timer 来源 | 1 条 `_timer` 线程按间隔往同一 Queue 塞 EVENT_TIMER | 1 条 timer 线程往主队列 `put(Timer)`，再由 main worker 在 dispatch_timer 里推一份到 strategy_ring_ |
| 策略注册方式 | OptionStrategyEngine 向 EventEngine 注册 3 个 handler；事件线程依次调用这些 handler，handler 内再按 strategy_name 分发到具体策略 | 无“向 EventEngine 注册 handler”；main worker 在 dispatch_* 里直接调 PositionEngine/HedgeEngine，并把 Timer/Order/Trade 推入 strategy_ring_；strategy 线程从 ring 取事件后调 OptionStrategyEngine 的 process_strategy，内部按 event 类型与 strategy_name 调各策略 |
| 其他线程 | Market data poll_thread、Gateway run() 等，与策略执行无关 | GatewayClient/MarketDataClient 的 sub_thread_、gRPC 池，同样与“谁跑策略”无关 |

**归纳**：v1_python 是**单队列 + 单事件处理线程**（该线程既做 infra 又调所有策略回调）；C++ Live 是**主队列 + 策略队列、main worker + strategy 线程**（infra 与策略回调拆成两条线程，但**策略侧仍是单线程**）。若要写成一句：**Live (C++) 与 v1_python 均采用单线程事件循环驱动多策略，而非每策略一线程**；C++ 只是把“基础设施”和“策略回调”拆成两个线程与两个队列，语义与 Python 一致。
