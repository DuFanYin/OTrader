# Otrader 架构设计文档

> 本文档从架构设计角度描述 Otrader（C++ 期权交易引擎）的层次划分、职责边界与数据流。

---

## Part 0. 文档说明与文件树

### 0.1 文档目标与范围

- **目标**：为开发与维护者提供清晰的架构视图，明确各模块职责、边界与协作方式。
- **范围**：仅描述 Otrader 文件夹内的 C++ 子系统，不与其他语言或后端做对比。
- **原则**：以「Event 入、Intent 出」为主线，区分 Domain Core、Runtime、Infrastructure 三层。

### 0.2 文件树

```
Otrader/
├── ARCHITECTURE_NEW.md      # 本文档
├── CMakeLists.txt
├── entry_backtest.cpp       # 回测可执行入口
├── entry_live.cpp           # 实盘可执行入口（无 gRPC）
├── entry_live_grpc.cpp      # 实盘 + gRPC 服务入口
│
├── runtime/                 # 运行时：回测与实盘差异集中在此
│   ├── backtest/
│   │   ├── CMakeLists.txt
│   │   ├── engine_backtest.{cpp,hpp}   # 回测顶层控制器
│   │   ├── engine_event.{cpp,hpp}      # 回测事件引擎（同步分发）
│   │   ├── engine_main.{cpp,hpp}       # 回测 MainEngine
│   │   └── engine_data_historical.hpp  # 见 infra，backtest 通过 main 引用
│   └── live/
│       ├── CMakeLists.txt
│       ├── engine_event.{cpp,hpp}      # 实盘事件引擎（队列 + 工作线程）
│       ├── engine_main.{cpp,hpp}       # 实盘 MainEngine
│       ├── engine_grpc.{cpp,hpp}       # gRPC 服务实现（持有 MainEngine*）
│       ├── engine_data_tradier.{cpp,hpp}   # 见 infra
│       ├── engine_db_pg.{cpp,hpp}          # 见 infra
│       └── engine_gateway_ib.{cpp,hpp}     # 见 infra
│
├── infra/                   # 基础设施：数据、持久化、网关
│   ├── marketdata/
│   │   ├── engine_data_historical.{cpp,hpp}  # 回测数据引擎（parquet → 快照）
│   │   └── engine_data_tradier.{cpp,hpp}     # 实盘行情/组合引擎（Contract + Snapshot）
│   ├── db/
│   │   └── engine_db_pg.{cpp,hpp}            # PostgreSQL 合约/订单/成交
│   └── gateway/
│       └── engine_gateway_ib.{cpp,hpp}       # IB TWS 网关
│
├── proto/
│   ├── otrader_engine.proto                   # gRPC 服务与消息定义
│   └── (生成) otrader_engine.pb.{cc,h}, otrader_engine.grpc.pb.{cc,h}
│
├── core/                    # 领域核心：策略引擎与辅助引擎（无 Context，caller 传参或 RuntimeAPI）
│   ├── CMakeLists.txt
│   ├── engine_option_strategy.{cpp,hpp}  # 统一策略引擎 + RuntimeAPI
│   ├── engine_position.{cpp,hpp}        # 策略持仓管理
│   ├── engine_hedge.{cpp,hpp}           # 对冲引擎（产出 orders/cancels/logs）
│   ├── engine_combo_builder.{cpp,hpp}    # 组合腿构造
│   ├── engine_log.{cpp,hpp}             # 日志引擎（消费 LogIntent）
│   └── log_sink.hpp
│
├── strategy/                # 策略实现与注册
│   ├── CMakeLists.txt
│   ├── template.{cpp,hpp}                # 策略模板基类
│   ├── high_frequency_momentum.{cpp,hpp} # 示例策略
│   ├── strategy_registry.{cpp,hpp}       # 策略类名 → 工厂
│   └── (其他策略...)
│
├── utilities/               # 通用数据模型与基础设施
│   ├── CMakeLists.txt
│   ├── constant.hpp         # 枚举、常量、to_string
│   ├── types.hpp            # 时间戳等类型
│   ├── object.{cpp,hpp}     # 订单、成交、合约、持仓、快照等结构体
│   ├── portfolio.{cpp,hpp}  # PortfolioData / ChainData / OptionData / IEventEngine
│   ├── event.hpp            # EventType / EventPayload / Event / StrategyUpdateData
│   ├── base_engine.hpp      # MainEngine 抽象、BaseEngine 基类
│   ├── utility.{cpp,hpp}
│   ├── parquet_loader.{cpp,hpp}
│   ├── occ_utils.{cpp,hpp}
│   ├── ib_mapping.hpp
│   ├── lets_be_rational_api.hpp
│   └── README.md
│
└── tests/
    ├── CMakeLists.txt
    ├── backtest/
    │   ├── test_backtest.cpp
    │   ├── test_backtest_data.cpp
    │   └── test_entry_multi.cpp
    └── live/
        └── test_live_components.cpp
```

说明：`runtime/live/` 通过 include 或链接使用 `infra/` 下的 marketdata、db、gateway；`runtime/backtest/` 使用 `infra/marketdata/engine_data_historical`（以及 utilities 的 parquet_loader 等）。proto 生成文件一般不纳入版本控制。

---

## Part 1. 系统概览

### 1.1 目标与定位

Otrader 是围绕**期权组合交易**场景构建的 C++20 交易引擎，承担两类核心职责：

1. **离线回测（Backtest）**  
   在单进程内完成历史数据回放、策略执行、订单撮合与绩效统计，对应可执行程序 `entry_backtest`。

2. **实时实盘（Live）**  
   通过 IB TWS 网关接收真实行情、下单并记录成交，可选暴露 gRPC 服务给上层后端，对应 `entry_live` 与 `entry_live_grpc`。

回测与实盘**共用同一套领域核心**（Core + Strategy）：统一策略引擎、持仓、对冲、组合构造与日志。差异仅体现在**运行时**：数据来源、时钟驱动、订单执行与持久化由各自的 Runtime 与 Infrastructure 实现。

### 1.2 核心设计理念

- **Event 入、Intent 出**  
  输入为事件流（Timer、Snapshot、Order、Trade、Contract）；输出为意图（下单、撤单、日志）。领域核心只消费 Event、只产出 Intent；执行由 Runtime 完成。

- **统一策略引擎**  
  `core::OptionStrategyEngine` 不区分 live/backtest，通过 **RuntimeAPI** 注入「发单、获取组合、合约、持仓、写日志」等能力，使策略逻辑与运行环境解耦。

- **事件驱动与固定分发顺序**  
  事件进入引擎后，在**同一处**按**写死顺序**调用各组件（Snapshot → 更新组合；Timer → 策略 on_timer、持仓指标、对冲、执行意图；Order/Trade → 更新状态）。无动态 handler 注册。

- **强类型数据模型**  
  使用结构体（ContractData、OrderData、TradeData、PortfolioSnapshot、StrategyHolding 等）描述合约、订单、持仓与组合，避免在关键路径传递松散键值结构。

- **可扩展的策略注册**  
  通过 `StrategyRegistry` 与 `REGISTER_STRATEGY` 宏集中注册策略类，Backtest 与 Live 均可按类名创建策略实例。

### 1.3 系统边界与对外接口

- **回测**：通过命令行启动（entry_backtest），标准输入/输出无约定；结果以 JSON 形式输出到 stdout，错误与进度可输出到 stderr。不依赖网络或数据库；所有输入来自命令行参数与 parquet 文件。
- **实盘（无 gRPC）**：通过 entry_live 启动；依赖 .env 中的 DATABASE_URL 等；与 IB TWS 通过 IbGateway 通信；无对外 RPC，仅进程内事件循环。
- **实盘（gRPC）**：通过 entry_live_grpc 启动；在 0.0.0.0:50051 暴露 EngineService；后端或其它客户端通过 gRPC 调用 GetStatus、ListStrategies、ConnectGateway、AddStrategy、StreamStrategyUpdates 等。合约与订单/成交持久化到 PostgreSQL，由 DatabaseEngine 与 load_contracts/save_order_data/save_trade_data 完成。

### 1.4 架构分层总览

| 层次 | 职责 | 主要组件 |
|------|------|----------|
| **Domain Core** | 纯逻辑：接收 Event、更新状态、输出 Intent；不直接下单、不读库、不访问网关 | OptionStrategyEngine、PositionEngine、HedgeEngine、ComboBuilderEngine、LogEngine（消费 Intent）、Strategy 实现 |
| **Runtime** | 接入数据与时钟，执行 Core 产出的 Intent，将执行结果转为 Event 回灌 | BacktestEngine、EventEngine（backtest/live）、MainEngine（backtest/live）、gRPC Service（live） |
| **Infrastructure** | 数据源、持久化、网关 | BacktestDataEngine、MarketDataEngine、DatabaseEngine、IbGateway |
| **Utilities** | 通用数据模型、事件与引擎抽象 | event.hpp、portfolio.hpp、object.hpp、base_engine.hpp、constant.hpp 等 |

下文按「职责边界 → 静态结构 → 事件与意图 → 运行时 → 数据流 → 扩展点」展开。

### 1.5 设计约束与原则（架构层面）

- **Core 不依赖 Runtime**：core/ 与 strategy/ 不包含对 backtest 或 live 的 #include；仅依赖 utilities 与 RuntimeAPI 的抽象（函数类型、PortfolioData*、ContractData* 等）。这样回测与实盘可独立替换 Runtime 与 Infra 实现。
- **单线程与多线程**：回测 EventEngine 同步执行，无队列无工作线程；实盘 EventEngine 单工作线程消费队列、单定时器线程投递 Timer。策略与 Core 引擎在 dispatch 调用栈内执行，无需自身考虑线程安全；MainEngine 持有的状态（如 strategy_updates_、log_stream_buffer_）若被 gRPC 线程访问，需加锁（当前实现中由 MainEngine 内 mutex/cv 保护）。
- **写死顺序**：事件处理顺序不在配置或注册表中，而在 EventEngine 的 process/put_event 分支与 dispatch_* 实现中写死。优点是可预测、易调试；扩展新事件类型时需改 EventEngine 与分支逻辑。
- **日志单一 sink**：LogEngine 仅一个 level 与一个 sink；所有日志意图经 put_log_intent 汇聚到 LogEngine，由 level 统一过滤。不区分「策略日志」与「系统日志」的通道，仅通过 level 与 msg 区分。

### 1.6 核心概念速查

- **Event**：进入引擎的输入，类型为 Timer/Snapshot/Order/Trade/Contract；Payload 为对应数据结构（PortfolioSnapshot、OrderData、TradeData、ContractData 等）。
- **Intent**：策略或 Hedge 产出的「意图」，包括 OrderRequest（下单）、CancelRequest（撤单）、LogData（日志）；由 Runtime 通过 append_order、append_cancel、put_log_intent 执行。
- **RuntimeAPI**：MainEngine 注入给 OptionStrategyEngine 的能力集合；策略与 OptionStrategyEngine 仅通过该 API 访问环境，不直接依赖 MainEngine 或 EventEngine。
- **dispatch**：EventEngine 根据事件类型调用固定顺序的处理逻辑（dispatch_snapshot、dispatch_timer、dispatch_order、dispatch_trade、dispatch_contract）；无动态注册的 handler。
- **apply_frame**：PortfolioData 的方法；将 PortfolioSnapshot 按 option_apply_order_ 顺序写回组合内各期权与标的的价格与 Greeks。
- **option_apply_order_**：PortfolioData 内固定好的期权指针顺序，与 PortfolioSnapshot 的向量一一对应；在 finalize_chains 或回测 build_option_apply_index 时确定，后续不变。
- **order_executor**：回测 MainEngine 持有的可调用对象，由 BacktestEngine::execute_order 充当；append_order 时调用，完成撮合并 put_event(Order/Trade) 回灌。

---

## Part 2. 职责边界

### 2.1 Domain 层（Core + Strategy）

#### 2.1.1 总体原则

- **无 Context**：Core 内各引擎不持有 IEventEngine 或 MainEngine；所需能力由 **caller 传参**或通过 **RuntimeAPI** 注入。
- **只读环境 + 产出 Intent**：策略与 Hedge 通过 API 读取组合、合约、持仓；通过 API 提交订单请求、撤单请求、日志。不直接调用网关或数据库。

#### 2.1.2 OptionStrategyEngine（core）

- **职责**：策略实例管理、订单/成交状态（OMS 视角）、将 RuntimeAPI 暴露给策略并驱动生命周期（on_init/on_start/on_stop/on_timer）。
- **依赖**：仅依赖 `RuntimeAPI`（由 MainEngine 构造时注入）。不持 MainEngine、不持 IEventEngine。
- **对外**：`process_order` / `process_trade` 由 EventEngine 在 dispatch_order / dispatch_trade 中调用；`on_timer()` 由 EventEngine 在 dispatch_timer 中调用。策略侧通过 `send_order` / `send_combo_order` / `write_log` 等走 RuntimeAPI。

#### 2.1.3 PositionEngine（core）

- **职责**：维护策略持仓（StrategyHolding）；处理订单与成交以更新持仓；按组合更新汇总指标（update_metrics）。日志以 LogData 追加到调用方提供的 vector，由调用方统一 put_log_intent。
- **依赖**：caller 传入 get_portfolio、portfolio 等；无执行类回调。
- **对外**：由 EventEngine 在 dispatch_timer 中调用 update_metrics（回测）或 process_timer_event（live）；在 dispatch_order / dispatch_trade 中调用 process_order / process_trade。

#### 2.1.4 HedgeEngine（core）

- **职责**：集中式 delta 等对冲逻辑；根据 HedgeParams（只读：portfolio、holding、get_contract、get_strategy_active_orders、get_order）产出 orders、cancels、logs，写入 caller 提供的 vector。
- **依赖**：纯只读 HedgeParams；无执行回调。执行由 Runtime 对产出的 orders/cancels/logs 调用 send_order/cancel_order/put_log_intent。
- **对外**：由 EventEngine 在 dispatch_timer 中组 HedgeParams、调用 process_hedging，再对结果逐条执行。

**HedgeParams 与 HedgeConfig**：HedgeParams 由 EventEngine 在 dispatch_timer 中组装：portfolio 来自 get_portfolio(portfolio_name)、holding 来自 get_holding(strategy_name)、get_contract/get_strategy_active_orders/get_order 来自 MainEngine 与 OptionStrategyEngine 的 lambda。HedgeConfig 按策略名注册（timer_trigger、delta_target、delta_range）；process_hedging 内部根据 config 与 params 计算对冲单并追加到 out_orders、out_cancels、out_logs。策略可通过 RuntimeAPI.get_hedge_engine() 与 register_hedging 注册自身参与对冲。

#### 2.1.5 ComboBuilderEngine（core）

- **职责**：按 ComboType 与期权数据生成标准化 Leg 与组合签名。纯函数风格；get_contract 由 caller 传入；日志追加到 out_logs。
- **依赖**：caller 传 get_contract、option_data 等。
- **对外**：策略通过 RuntimeAPI.get_combo_builder_engine() 取得后调用；out_logs 由调用方 write_log/append_log 打出。

#### 2.1.6 LogEngine（core）

- **职责**：消费 LogIntent（process_log_intent）；按 level 过滤后输出到 sink。单一 sink，由 LogEngine 的 level 统一控制。
- **依赖**：仅持 MainEngine*（用于写日志时的 gateway 等）；不持 IEventEngine。
- **对外**：MainEngine 的 put_log_intent/append_log 将 LogData 交给 LogEngine；MainEngine 暴露 set_log_level/log_level。

#### 2.1.7 Strategy 层（strategy/）

- **职责**：实现具体策略逻辑（派生 OptionStrategyTemplate）；在 on_init_logic、on_timer_logic 等中读组合/持仓、产生产单/撤单/日志意图。
- **依赖**：仅通过 OptionStrategyEngine（即 RuntimeAPI）访问环境；不直接接触 EventEngine 或 MainEngine。
- **注册**：StrategyRegistry 维护类名 → 工厂；Backtest/Live 通过类名创建实例并交给 OptionStrategyEngine 管理。

#### 2.1.8 OptionStrategyEngine 内部状态（OMS 视角）

- **strategies_**：策略名 → 策略实例（OptionStrategyTemplate 派生类）。
- **orders_** / **trades_**：orderid/tradeid → OrderData/TradeData；用于查询与策略侧 get_order/get_trade。
- **strategy_active_orders_**：策略名 → 该策略当前活跃 orderid 集合；用于对冲等逻辑判断订单是否仍挂单。
- **orderid_strategy_name_**：orderid → strategy_name；用于 live 侧 dispatch_order/dispatch_trade 后根据 orderid 取策略名以 save_order_data/save_trade_data。
- **all_active_order_ids_**：当前所有未完结订单 id；回测 MainEngine 在 append_order 后可能插入 orderid，cancel 时通过 remove_order_tracking 移除。

策略与 Hedge 不直接访问上述集合；通过 get_order、get_trade、get_strategy_active_orders、get_strategy_name_for_order 等接口访问。

#### 2.1.9 RuntimeAPI 职责汇总

RuntimeAPI 由 MainEngine（backtest 或 live）在构造时组装并注入 OptionStrategyEngine。策略与 OptionStrategyEngine 内部仅通过该 API 访问环境，不持有 MainEngine 或 IEventEngine。

| API 成员 | 职责 | 回测实现 | 实盘实现 |
|----------|------|----------|----------|
| send_order | 提交订单请求 | append_order → order_executor 撮合 | append_order → IbGateway::send_order |
| send_combo_order | 提交组合订单 | 同 send_order（构造 OrderRequest 后 append_order） | 同 send_order |
| write_log | 提交日志意图 | put_log_intent → LogEngine | put_log_intent → LogEngine |
| get_portfolio | 按名称取组合视图 | MainEngine::get_portfolio（来自 BacktestDataEngine 注册） | MainEngine::get_portfolio（来自 MarketDataEngine） |
| get_contract | 按 symbol 取合约 | MainEngine::get_contract（来自 BacktestDataEngine 注册） | MainEngine::get_contract（来自 MarketDataEngine） |
| get_holding | 按策略名取持仓 | MainEngine::get_holding（PositionEngine） | MainEngine::get_holding |
| get_or_create_holding | 确保策略持仓存在 | MainEngine::get_or_create_holding | MainEngine::get_or_create_holding |
| remove_strategy_holding | 移除策略持仓 | PositionEngine::remove_strategy_holding | 同上 |
| get_combo_builder_engine | 取组合构造引擎 | MainEngine::combo_builder_engine() | MainEngine::combo_builder_engine() |
| get_hedge_engine | 取对冲引擎 | MainEngine::hedge_engine() | MainEngine::hedge_engine() |
| put_strategy_event | 推送策略更新（供 gRPC 流） | 可选 no-op 或空实现 | MainEngine::on_strategy_event 入队 |

#### 2.1.10 PositionEngine 与 StrategyHolding

- **StrategyHolding**：每个策略一份；内含标的持仓、期权持仓、组合持仓及 PortfolioSummary（pnl、delta、gamma、theta 等汇总）。由 PositionEngine 维护 strategy_holdings_ 映射。
- **process_order**：根据 OrderData 更新 order_meta_（用于后续 process_trade 时识别组合腿）；不直接改持仓，持仓由 process_trade 更新。
- **process_trade**：根据成交更新对应 StrategyHolding 内标的/期权/组合持仓数量与成本；更新 summary。
- **update_metrics**：根据当前 PortfolioData 与持仓重新计算 StrategyHolding 的 summary（Greeks、pnl 等）。
- **process_timer_event**（仅 live）：可选产出 pos_logs，由调用方 put_log_intent；用于定时检查持仓或风控日志。
- **get_or_create_holding**：MainEngine 通过 position_engine->get_create_strategy_holding 或类似接口确保策略有对应 StrategyHolding；OptionStrategyEngine 通过 RuntimeAPI.get_or_create_holding 调用到 MainEngine，最终落到 PositionEngine。

### 2.2 Runtime 层（Backtest / Live）

#### 2.2.1 共同点

- **MainEngine**：持有各引擎（OptionStrategyEngine、PositionEngine、HedgeEngine、ComboBuilderEngine、LogEngine 等）；提供 send_order、cancel_order、put_log_intent、get_portfolio、get_contract、get_holding 等；**不包含** dispatch 控制逻辑。
- **EventEngine**：负责按事件类型与固定顺序分发（dispatch_snapshot、dispatch_timer、dispatch_order、dispatch_trade，live 另有 dispatch_contract）；不持有引擎，通过 MainEngine 的 accessor 访问并调用；执行（send_order/cancel_order/put_log_intent）也通过 MainEngine。
- **put_event**：MainEngine 的 put_event 委托给 EventEngine.put_event；回测为同步调用，live 为入队后由工作线程 process。

#### 2.2.2 回测特有

- **BacktestEngine**：顶层控制器；持有 MainEngine 与 EventEngine；在 run() 中按时间步先 put_event(Snapshot)，再 put_event(Timer)；通过 set_order_executor 将自身 execute_order 注入 MainEngine，用于撮合。
- **Backtest MainEngine**：无网关与数据库；持有 BacktestDataEngine；append_order 内部调用 order_executor（即 BacktestEngine::execute_order），撮合后 add_order、put_event(Order)、put_event(Trade) 回灌。
- **Backtest EventEngine**：无队列无线程；put_event 直接按类型分支并同步执行 dispatch_*。

#### 2.2.3 实盘特有

- **Live MainEngine**：持有 DatabaseEngine、MarketDataEngine、IbGateway；构造时调用 db_engine_->load_contracts()，通过 put_event(Contract) 驱动 MarketDataEngine 建立组合结构；append_order/append_cancel 转 IbGateway；dispatch_order/dispatch_trade 内调用 save_order_data/save_trade_data。
- **Live EventEngine**：队列 + 工作线程 + 定时器线程；put(event) 入队，run() 中取事件并 process(event)；run_timer 按间隔 put(Timer)。
- **gRPC**：GrpcLiveEngineService 持有 MainEngine*，各 RPC 直接调用 MainEngine 接口（GetStatus、ListStrategies、ConnectGateway、AddStrategy、StreamStrategyUpdates 等）。

#### 2.2.4 MainEngine 与 EventEngine 的职责划分

- **MainEngine**：只负责「持有引擎实例」与「提供能力接口」；不包含「按事件类型决定先调谁、后调谁」的逻辑。put_event 仅转发给 EventEngine；append_order/append_cancel/append_log 内部转 send_order/cancel_order/put_log_intent。
- **EventEngine**：只负责「接收事件」与「按固定顺序分发」；不持有任何引擎实例，通过 set_main_engine 获得的 MainEngine* 访问 get_portfolio、option_strategy_engine、position_engine、hedge_engine、send_order、cancel_order、put_log_intent 等。执行意图（下单、撤单、写日志）一律通过 MainEngine 完成。

因此：**dispatch 控制逻辑**与**引擎持有与执行**分离；EventEngine 是「调度器」，MainEngine 是「容器与执行门面」。

#### 2.2.5 回测 MainEngine 与实盘 MainEngine 对比

| 能力 | 回测 MainEngine | 实盘 MainEngine |
|------|-----------------|-----------------|
| 事件引擎 | 持有 IEventEngine*（Backtest EventEngine），同步 put_event | 持有或使用 EventEngine*，put_event 入队 |
| 组合/合约来源 | BacktestDataEngine load 时 register_portfolio、register_contract | MarketDataEngine 经 process_contract 建立；合约来自 load_contracts |
| 订单执行 | append_order → order_executor（BacktestEngine::execute_order）撮合 | append_order → send_order → IbGateway::send_order |
| 撤单 | cancel_order：更新状态、remove_order_tracking、put_event(Order) | cancel_order → IbGateway::cancel_order |
| 持久化 | 无 | save_order_data、save_trade_data → DatabaseEngine |
| 网关 | 无 | connect/disconnect、query_account、query_position → IbGateway |
| 策略更新流 | 无（可选 no-op） | on_strategy_event、pop_strategy_update 供 gRPC StreamStrategyUpdates |
| 日志流 | 无 | log_stream_buffer_、pop_log_for_stream 供 gRPC StreamLogs |
| 初始化后加载 | load_backtest_data 由 BacktestEngine 调用 | 构造末 db_engine_->load_contracts() |

### 2.3 基础设施层（infra）

- **BacktestDataEngine**（marketdata）：从 parquet 加载历史数据；构建回测用 PortfolioData；预计算每帧 PortfolioSnapshot；提供 iter_timesteps 与 get_precomputed_snapshot。回测组合结构在 load 时建立，行情由每步 Snapshot 事件更新。
- **MarketDataEngine**（engine_data_tradier）：处理 Contract 事件，维护 contracts_、portfolios_（add_option/set_underlying），add_option 后 finalize_chains 以便 apply_frame 可用；不直接产生行情，行情由外部 Snapshot 事件经 dispatch_snapshot → get_portfolio()->apply_frame(snapshot) 更新。
- **DatabaseEngine**（db）：PostgreSQL；load_contracts 按固定顺序发出 Contract 事件；save_order_data/save_trade_data 在 dispatch_order/dispatch_trade 中被调用。
- **IbGateway**（gateway）：封装 IB TWS 连接；send_order/cancel_order 下发；订单/成交/合约回报通过 main_engine->put_event(Order/Trade/Contract) 回灌；process_timer_event 用于周期性消费 TWS 消息队列。

#### 2.3.1 BacktestDataEngine 与回测数据流

- **输入**：parquet 文件路径、时间列名（默认 ts_recv）、标的符号（可选，可从文件名推断）。
- **过程**：load_parquet 解析 parquet，构建标的与期权合约列表；create_portfolio_data 创建 PortfolioData 并注册到 MainEngine；build_occ_to_option、build_option_apply_index 建立 OCC 符号到 OptionData* 的映射与 apply 顺序；precompute_snapshots 逐帧构建 PortfolioSnapshot 并写入 snapshots_。
- **输出**：iter_timesteps 提供 (timestamp, TimestepFrameColumnar) 回调；get_precomputed_snapshot(step) 提供第 step 帧快照；portfolio_data() 返回供 MainEngine 注册的 PortfolioData*。回测组合结构在 load 阶段定型，运行时仅通过 apply_frame 更新价格与 Greeks。

- **TimestepFrameColumnar 与 precompute**：每帧对应 parquet 中按时间列（如 ts_recv）分组的一批行；build_snapshot_from_frame 根据当前帧列数据填充 PortfolioSnapshot 的 underlying 与 option 向量，顺序与 option_apply_order_ 一致；precompute_snapshots 在 load 结束时一次性生成所有帧的快照，避免运行时逐帧解析。

#### 2.3.2 MarketDataEngine（实盘）与组合结构

- **输入**：Contract 事件（EventPayload 为 ContractData）；来自 DatabaseEngine::load_contracts 按固定顺序发出。
- **过程**：process_contract 中根据 contract 的 option_portfolio、option_underlying 等字段 get_or_create_portfolio、add_option、set_underlying；每次 add_option 后调用 finalize_chains，保证 option_apply_order_ 与链内顺序稳定。
- **输出**：get_portfolio、get_contract、get_all_portfolio_names、get_all_contracts 供 MainEngine 与策略使用。行情/Greeks 不由此引擎产生，由外部 Snapshot 事件经 dispatch_snapshot → get_portfolio(name)->apply_frame(snapshot) 更新。

#### 2.3.3 DatabaseEngine 与合约加载顺序

- load_contracts 从 PostgreSQL 读取合约表，按约定顺序（如先标的后期权）遍历，对每条构造 ContractData 并 put_event(EventType::Contract, data)。EventEngine 工作线程 process 时 dispatch_contract → MarketDataEngine::process_contract，从而建立 portfolios_。该顺序需与 MarketDataEngine 对 option_apply_order_ 的假设一致，以便后续 Snapshot 的 apply_frame 正确写回。

### 2.4 通用层（utilities）

- **数据与枚举**：constant.hpp、types.hpp、object.hpp（OrderRequest、OrderData、TradeData、ContractData、PortfolioSnapshot、StrategyHolding 等）、portfolio.hpp（PortfolioData、ChainData、OptionData、UnderlyingData）。
- **事件抽象**：event.hpp（EventType、EventPayload、Event）、portfolio.hpp（IEventEngine）。
- **引擎抽象**：base_engine.hpp（MainEngine 虚接口、BaseEngine 基类）。
- **工具**：parquet_loader、occ_utils、utility、ib_mapping 等。

#### 2.4.1 事件与引擎抽象（utilities）

- **Event**：type（Timer/Order/Trade/Contract/Snapshot）+ data（variant<monostate, OrderData, TradeData, ContractData, PortfolioSnapshot>）。全系统唯一的事件载体，回测与实盘共用。
- **IEventEngine**：最小事件引擎接口。start/stop、register_handler/unregister_handler（默认 no-op）、put_intent_send_order、put_intent_cancel_order、put_intent_log、put_event。回测与 live 的 EventEngine 实现该接口；策略与 Core 不直接依赖 IEventEngine，只通过 RuntimeAPI 提交意图。
- **MainEngine（虚）**：write_log、put_event 两个虚方法，默认 no-op。backtest::MainEngine 与 engines::MainEngine 重写后委托给各自 EventEngine 或 LogEngine。
- **BaseEngine**：持 MainEngine*、engine_name；close 钩子。所有「功能引擎」（LogEngine、PositionEngine、MarketDataEngine、DatabaseEngine 等）继承 BaseEngine，**不持 IEventEngine**。事件入口统一由 EventEngine 持有并调用各引擎（通过 MainEngine 的 accessor）；引擎自身不订阅或拉取事件，仅在被 dispatch 或 MainEngine 直接调用时工作。

#### 2.4.2 组合与快照（utilities/portfolio.hpp、object.hpp）

- **PortfolioData**：组合顶层；name、options、chains、underlying、underlying_symbol、option_apply_order_（固定顺序，供 apply_frame 写回）。方法：add_option、set_underlying、finalize_chains、apply_frame(snapshot)、get_chain 等。
- **ChainData**：单链；chain_symbol、underlying、options、calls、puts、indexes、atm_price、days_to_expiry 等。
- **OptionData / UnderlyingData**：单期权/标的视图；价格、Greeks、链与组合指针。
- **PortfolioSnapshot**：紧凑快照；portfolio_name、datetime、underlying_bid/ask/last、以及与 option_apply_order 一一对应的 bid/ask/last/delta/gamma/theta/vega/iv 向量。用于 apply_frame 批量更新组合状态。

---

## Part 3. 静态结构

### 3.1 目录与模块映射

| 目录/文件 | 所属层次 | 职责摘要 |
|-----------|----------|----------|
| entry_backtest.cpp | Runtime | 解析命令行（parquet、策略名、fill_mode、fee_rate、slippage_bps、risk_free_rate、iv_price_mode、log、策略 key=value），构建 BacktestEngine，load_backtest_data、add_strategy、configure_execution、run()，汇总 BacktestResult 以 JSON 输出 stdout |
| entry_live.cpp | Runtime | 信号处理、.env、创建 EventEngine+MainEngine，connect，主循环，disconnect/close |
| entry_live_grpc.cpp | Runtime | 创建 EventEngine+MainEngine，GrpcLiveEngineService，gRPC Server 阻塞，退出时 disconnect/close |
| runtime/backtest/engine_backtest | Runtime | 回测顶层：load_backtest_data、add_strategy、configure_execution、run、execute_order、timestep 回调 |
| runtime/backtest/engine_main | Runtime | 回测 MainEngine：持有各引擎、RuntimeAPI 注入、register_portfolio/contract、append_*、set_order_executor |
| runtime/backtest/engine_event | Runtime | 回测 EventEngine：put_event 同步分发 Snapshot/Timer/Order/Trade |
| runtime/live/engine_main | Runtime | 实盘 MainEngine：持有各引擎与 infra 组件、load_contracts、append_*、put_event、策略更新队列 |
| runtime/live/engine_event | Runtime | 实盘 EventEngine：队列+线程、put 入队、process 分发 Snapshot/Timer/Order/Trade/Contract |
| runtime/live/engine_grpc | Runtime | gRPC 服务实现，持有 MainEngine*，各 RPC 转调 MainEngine |
| infra/marketdata/engine_data_historical | Infra | 回测数据：parquet 加载、组合构建、预计算快照、iter_timesteps |
| infra/marketdata/engine_data_tradier | Infra | 实盘行情/组合：process_contract 建组合、subscribe_chains、start/stop_market_data_update |
| infra/db/engine_db_pg | Infra | PostgreSQL：load_contracts、save/load contract、save order/trade、wipe_trading_data |
| infra/gateway/engine_gateway_ib | Infra | IB 网关：connect、send_order、cancel_order、回报回调 put_event |
| core/engine_option_strategy | Core | 统一策略引擎 + RuntimeAPI 定义与注入 |
| core/engine_position | Core | 策略持仓、process_order/process_trade、update_metrics、process_timer_event |
| core/engine_hedge | Core | 对冲：process_hedging 产出 orders/cancels/logs |
| core/engine_combo_builder | Core | 组合腿构造：combo_builder、straddle、strangle 等 |
| core/engine_log | Core | 消费 LogIntent、level 过滤、sink 输出 |
| strategy/template | Core/Strategy | 策略基类 OptionStrategyTemplate |
| strategy/strategy_registry | Core/Strategy | 策略类名 → 工厂、REGISTER_STRATEGY 宏 |
| strategy/high_frequency_momentum | Strategy | 示例策略实现 |
| utilities/* | Utilities | 事件、组合、对象、常量、基类、工具 |

### 3.2 组件依赖关系（高层）

- **BacktestEngine** → EventEngine、MainEngine（backtest）；MainEngine 持有 OptionStrategyEngine、PositionEngine、BacktestDataEngine、HedgeEngine、ComboBuilderEngine、LogEngine。
- **Live MainEngine** → EventEngine、OptionStrategyEngine、PositionEngine、MarketDataEngine、DatabaseEngine、IbGateway、HedgeEngine、ComboBuilderEngine、LogEngine。
- **OptionStrategyEngine** → 仅 RuntimeAPI（由 MainEngine 注入）；策略实例通过 RuntimeAPI 访问 portfolio、contract、holding、send_order、write_log 等。
- **EventEngine** → 通过 MainEngine* 访问 get_portfolio、option_strategy_engine、position_engine、hedge_engine、send_order、cancel_order、put_log_intent、market_data_engine（live）、save_order_data/save_trade_data（live）等。

### 3.2.1 头文件依赖关系（高层）

- **utilities**：event.hpp、object.hpp、constant.hpp、portfolio.hpp、base_engine.hpp、types.hpp 等仅依赖标准库与彼此，不依赖 core、runtime、infra。
- **core**：engine_option_strategy、engine_position、engine_hedge、engine_combo_builder、engine_log 依赖 utilities 与彼此（如 OptionStrategyEngine 依赖 RuntimeAPI 与 strategy_cpp 前向声明）；不依赖 runtime 或 infra。
- **strategy**：template、strategy_registry、high_frequency_momentum 依赖 core（OptionStrategyEngine）、utilities（PortfolioData、ContractData 等）；不依赖 runtime 或 infra。
- **runtime/backtest**：engine_backtest、engine_main、engine_event 依赖 core、utilities、infra/marketdata/engine_data_historical（BacktestDataEngine）。
- **runtime/live**：engine_main、engine_event、engine_grpc 依赖 core、utilities、infra 下 marketdata、db、gateway，以及 proto 生成代码。
- **infra**：各 engine 依赖 utilities、core/engine_log（若写日志）；不依赖 runtime。

依赖方向为：utilities ← core/strategy ← infra ← runtime（entry 依赖 runtime）。

### 3.3 Utilities 层关键类型（object.hpp、constant.hpp）

以下类型为全系统共享的数据契约，不包含业务逻辑，仅作数据结构定义。

| 类型 | 用途 |
|------|------|
| Direction | LONG / SHORT / NET |
| Status | 订单状态：SUBMITTING、NOTTRADED、PARTTRADED、ALLTRADED、CANCELLED、REJECTED |
| Product / Exchange / OrderType / OptionType / ComboType | 产品、交易所、订单类型、期权类型、组合类型等枚举 |
| ContractData | 合约元数据：symbol、exchange、乘数、tick、期权字段（strike、expiry、option_portfolio、option_underlying 等）、IB con_id/trading_class |
| OrderRequest | 下单意图：symbol、direction、price、volume、type、is_combo、legs 等 |
| OrderData | 订单状态：orderid、status、traded、volume、组合腿信息等 |
| TradeData | 成交记录：tradeid、orderid、symbol、price、volume、direction、datetime 等 |
| CancelRequest | 撤单意图 |
| Leg | 组合腿：con_id、symbol、direction、ratio、price、trading_class 等 |
| BasePosition / OptionPositionData / UnderlyingPositionData / ComboPositionData | 持仓基类与派生：数量、成本、价格、Greeks 等 |
| StrategyHolding | 策略持仓聚合：标的持仓、期权持仓、组合持仓、PortfolioSummary（pnl、delta、gamma、theta 等） |
| PortfolioSnapshot | 见 2.4.2 |
| LogData | 日志载荷：msg、level、gateway_name、time |
| TickData / OptionMarketData / ChainMarketData | 行情与链行情（部分路径使用） |

### 3.4 命名空间与模块归属

- **utilities**：event.hpp、portfolio.hpp、object.hpp、base_engine.hpp、constant.hpp、types.hpp 等；全系统共享类型与最小抽象。
- **backtest**：runtime/backtest 下 BacktestEngine、EventEngine、MainEngine；仅回测使用。
- **engines**：runtime/live 下 MainEngine、EventEngine、GrpcLiveEngineService；core 下 PositionEngine、HedgeEngine、ComboBuilderEngine、LogEngine；infra 下 MarketDataEngine、DatabaseEngine、IbGateway。engines 命名空间为 live 与 core 共用，便于区分 utilities 与 backtest。
- **core**：OptionStrategyEngine、RuntimeAPI；被 backtest 与 live 的 MainEngine 共同依赖。
- **strategy_cpp**：OptionStrategyTemplate、StrategyRegistry；策略基类与注册表，被 core 与 strategy 实现共用。

### 3.5 策略模板生命周期（strategy/template）

- **OptionStrategyTemplate**：基类持有 engine_（OptionStrategyEngine*）、strategy_name_、portfolio_name_、portfolio_、underlying_、holding_、chain_map_、inited_、started_、error_ 等。
- **生命周期**：on_init（置 inited_，调 on_init_logic）→ on_start（置 started_）→ on_timer（由 EventEngine dispatch_timer 驱动，调 on_timer_logic）→ on_stop（调 on_stop_logic）。
- **环境访问**：portfolio()、underlying()、holding()、get_chain() 来自 engine_ 注入的 RuntimeAPI（get_portfolio、get_holding）；下单通过 engine_->send_order/send_combo_order；日志通过 engine_->write_log；组合腿通过 get_combo_builder_engine()、对冲通过 get_hedge_engine() 与 register_hedging。

#### 3.5.1 从 add_strategy 到 on_timer 的链路（概念）

1. **添加策略**：BacktestEngine::add_strategy(strategy_name, setting) 或 gRPC AddStrategy → MainEngine 侧 OptionStrategyEngine::add_strategy(class_name, portfolio_name, setting)。add_strategy 内部通过 StrategyRegistry::create(class_name, engine, strategy_name, portfolio_name, setting) 创建策略实例，并插入 strategies_；get_or_create_holding 确保该策略有 StrategyHolding。
2. **初始化与启动**：回测在 run() 前若策略未 inited 则调用 strategy->on_init()、strategy->on_start()；实盘可通过 gRPC InitStrategy、StartStrategy 触发。
3. **定时驱动**：回测每步 put_event(Timer)；Live 定时器线程 put(Timer)。EventEngine 在 dispatch_timer 中调用 option_strategy_engine->get_strategy()->on_timer()（回测单策略）或 option_strategy_engine->on_timer()（Live 遍历所有策略）。on_timer 内部调用 on_timer_logic()，策略在此读取 portfolio、holding，可能调用 send_order/send_combo_order/write_log。
4. **意图执行**：send_order 经 RuntimeAPI.send_order → MainEngine.append_order → send_order（回测为 order_executor，Live 为 IbGateway）；回报再以 Order/Trade 事件回灌，dispatch_order/dispatch_trade 更新 OptionStrategyEngine 与 PositionEngine。

---

## Part 4. 事件与意图

### 4.1 Event 形态与类型

- **定义位置**：utilities/event.hpp、object.hpp（Payload 中使用的类型）、portfolio.hpp（IEventEngine）。
- **EventType**：Timer、Order、Trade、Contract、Snapshot。
- **EventPayload**：variant<monostate, OrderData, TradeData, ContractData, PortfolioSnapshot>。
- **Event**：type + data。
- **StrategyUpdateData**：用于 live gRPC 流式输出策略更新，不经过 EventEngine 路由；结构含 strategy_name、class_name、portfolio、json_payload。

**无独立 Log 事件类型**：日志以 Intent（LogData）经 put_log_intent/append_log 提交，由 MainEngine 转 LogEngine 处理。

#### 4.1.1 EventPayload 各类型说明

- **OrderData**：订单状态；含 orderid、status、traded、volume、symbol、direction、gateway_name、组合腿等；dispatch_order 时更新 OptionStrategyEngine 与 PositionEngine，live 侧并写入 DB。
- **TradeData**：成交记录；含 tradeid、orderid、symbol、price、volume、direction、datetime；dispatch_trade 时更新 OptionStrategyEngine 与 PositionEngine、并写入持仓与 summary，live 侧并 save_trade_data。
- **ContractData**：合约元数据；含 symbol、exchange、乘数、期权字段（strike、expiry、option_portfolio、option_underlying、option_index 等）、IB 相关字段；dispatch_contract 时用于 MarketDataEngine 建组合。
- **PortfolioSnapshot**：紧凑快照；portfolio_name、datetime、underlying_bid/ask/last、与 option_apply_order 同序的 bid/ask/last/delta/gamma/theta/vega/iv 向量；dispatch_snapshot 时 get_portfolio(name)->apply_frame(snapshot) 写回组合内各 OptionData 与 UnderlyingData。

### 4.2 各事件类型职责

| 类型 | 含义 | 生产者 | 消费者（dispatch） |
|------|------|--------|---------------------|
| Timer | 时钟/周期驱动 | 回测：run 每步 put；Live：定时器线程 put | dispatch_timer：策略 on_timer、持仓、对冲、执行意图 |
| Snapshot | 组合快照（价格/Greeks） | 回测：BacktestDataEngine 预计算每步 put；Live：外部数据源 put | dispatch_snapshot → get_portfolio(name)->apply_frame(snapshot) |
| Order | 订单状态更新 | 回测：execute_order 后 put_event(Order)；Live：IbGateway 回报 put_event | dispatch_order → position_engine->process_order；option_strategy_engine->process_order；live 侧 save_order_data |
| Trade | 成交回报 | 回测：execute_order 后 put_event(Trade)；Live：IbGateway 回报 put_event | dispatch_trade → option_strategy_engine->process_trade；live 侧 save_trade_data；position_engine->process_trade |
| Contract | 合约信息 | Live：DatabaseEngine::load_contracts 按序 put | dispatch_contract → market_data_engine->process_contract |

### 4.3 Intent 形态与提交

- **OrderIntent**：OrderRequest 经 RuntimeAPI.send_order 或 send_combo_order 提交；Runtime 实现为 append_order → send_order（回测为 order_executor 撮合，live 为 IbGateway）。
- **CancelIntent**：CancelRequest 经 append_cancel → cancel_order。
- **LogIntent**：LogData 经 RuntimeAPI.write_log 或 put_log_intent/append_log 提交；最终由 LogEngine.process_log_intent 消费，按 level 过滤后输出。

策略与 Hedge 只通过 RuntimeAPI 或 MainEngine 的 append_* / put_log_intent 提交意图；不直接调用 IEventEngine.put_intent_*（EventEngine 内部可将 put_intent_* 转 main_engine->send_order/cancel_order/put_log_intent）。

### 4.4 事件分发顺序（写死）

**回测 EventEngine.put_event**：按 event.type 分支；同一事件内顺序为：

- Snapshot → dispatch_snapshot（仅更新组合）
- Timer → dispatch_timer：1）option_strategy_engine->get_strategy()->on_timer()；2）position_engine->update_metrics；3）hedge_engine->process_hedging；4）对 out_orders/out_cancels/out_logs 逐条 main_engine_->send_order/cancel_order/put_log_intent
- Order → dispatch_order：position_engine->process_order；option_strategy_engine->process_order
- Trade → dispatch_trade：position_engine->process_trade；option_strategy_engine->process_trade

**Live EventEngine.process(event)**：按 event.type 分支：

- Snapshot → dispatch_snapshot（同上）
- Timer → dispatch_timer：1）ib_gateway->process_timer_event(Timer)；2）position_engine->process_timer_event(get_portfolio, &pos_logs)，对 pos_logs 逐条 put_log_intent；3）每策略 process_hedging，对 orders/cancels/logs 逐条 send_order/cancel_order/put_log_intent；4）option_strategy_engine->on_timer()
- Order → dispatch_order：position_engine->process_order；option_strategy_engine->process_order；save_order_data(strategy_name, order)
- Trade → dispatch_trade：option_strategy_engine->process_trade；save_trade_data；position_engine->process_trade
- Contract → dispatch_contract：market_data_engine->process_contract

register_handler / unregister_handler 为 no-op，不改变上述顺序。

### 4.4.1 回测 EventEngine 与 Live EventEngine 对比

| 维度 | 回测 EventEngine | Live EventEngine |
|------|------------------|------------------|
| 线程模型 | 无独立线程；put_event 同步执行 process | 单工作线程 run() 消费队列；单定时器线程 run_timer() 按间隔 put(Timer) |
| 事件入口 | put_event 直接 switch 分支并调用 dispatch_* | put(event) 入队，工作线程从队列取事件后 process(event) 再 dispatch_* |
| 事件类型 | Snapshot、Timer、Order、Trade（无 Contract） | Snapshot、Timer、Order、Trade、Contract |
| 访问引擎 | 通过 set_main_engine 获得的 MainEngine* 访问 get_portfolio、option_strategy_engine、position_engine、hedge_engine、send_order、cancel_order、put_log_intent | 同上；另可访问 market_data_engine（dispatch_contract）、save_order_data、save_trade_data |
| register_handler | no-op，返回 0 | no-op，返回 0 |

### 4.5 回测与实盘 dispatch_timer 顺序对比

| 步骤 | 回测 dispatch_timer | Live dispatch_timer |
|------|---------------------|----------------------|
| 1 | option_strategy_engine->get_strategy()->on_timer() | ib_gateway->process_timer_event(Timer) |
| 2 | position_engine->update_metrics(strategy_name, portfolio) | position_engine->process_timer_event(get_portfolio, &pos_logs)；对 pos_logs put_log_intent |
| 3 | hedge_engine->process_hedging(..., out_orders, out_cancels, out_logs) | 对每策略：hedge_engine->process_hedging；对 orders/cancels/logs 逐条 send_order/cancel_order/put_log_intent |
| 4 | 对 out_orders/out_cancels/out_logs 逐条 send_order/cancel_order/put_log_intent | option_strategy_engine->on_timer() |

回测为单策略，get_strategy() 直接返回当前策略；Live 为多策略，先按策略执行对冲再统一 on_timer。

### 4.6 Log 路径统一

- 策略或 Hedge 写日志：RuntimeAPI.write_log(log) 或 engine_->write_log(msg) → MainEngine.put_log_intent(log) → LogEngine.process_log_intent(data)。
- LogEngine：仅当 data.level >= level_ 时输出；level 由 MainEngine::set_log_level 设置，回测与实盘一致。DISABLED(99) 可关闭全部输出。

### 4.7 Intent 在回测与 Live 中的执行路径

| Intent | 提交入口 | 回测执行路径 | Live 执行路径 |
|--------|----------|--------------|---------------|
| 下单 | RuntimeAPI.send_order → append_order | append_order → order_executor(req) → execute_order：撮合、add_order、put_event(Order)、put_event(Trade) | append_order → MainEngine::send_order(req) → IbGateway::send_order(req)；TWS 回报 → put_event(Order/Trade) |
| 撤单 | append_cancel(req) | MainEngine::cancel_order：remove_order_tracking、更新订单状态、put_event(Order) | MainEngine::cancel_order → IbGateway::cancel_order |
| 日志 | RuntimeAPI.write_log 或 put_log_intent | put_log_intent → LogEngine.process_log_intent | 同上 |
| Hedge 产出 | dispatch_timer 内 process_hedging 得到 out_orders/out_cancels/out_logs | 对 out_* 逐条 main_engine_->send_order/cancel_order/put_log_intent | 同上 |

策略与 Hedge 不直接调用 IbGateway 或 order_executor；一律通过 MainEngine 的 append_order/append_cancel/put_log_intent（或 RuntimeAPI 封装的 send_order、write_log）进入上述路径。

---

## Part 5. 运行时视图

### 5.1 回测运行时

**初始化**

1. entry_backtest 解析命令行（parquet 路径、策略名、fill_mode、fee_rate、slippage_bps、risk_free_rate、iv_price_mode、log、策略参数）。
2. 构建 BacktestEngine；其内部创建 EventEngine 与 MainEngine（backtest），EventEngine.set_main_engine(main_engine)，MainEngine.set_order_executor(BacktestEngine::execute_order)。
3. MainEngine 构造时组装 RuntimeAPI 并创建 OptionStrategyEngine、PositionEngine、LogEngine（默认 DISABLED）、HedgeEngine、ComboBuilderEngine；不创建 BacktestDataEngine（由 load_backtest_data 触发）。

**数据与策略加载**

1. BacktestEngine::load_backtest_data(parquet_path, underlying_symbol) → MainEngine::load_backtest_data → BacktestDataEngine::load_parquet；构建 portfolio_data_、预计算 snapshots_、向 MainEngine 注册 portfolio 与 contract。
2. BacktestEngine::add_strategy(strategy_name, setting) → OptionStrategyEngine::add_strategy(strategy_name, "backtest", setting)；通过 StrategyRegistry 创建策略实例并加入引擎。

**运行循环**

1. BacktestEngine::run() 校验 data_engine 与 strategy 存在，若策略未 init 则 on_init/on_start。
2. data_engine->iter_timesteps(callback)：对每一时间步：
   - event_engine_->put_event(Snapshot, data_engine->get_precomputed_snapshot(step_count))
   - event_engine_->put_event(Timer)
   - EventEngine 同步执行 dispatch_snapshot（apply_frame）→ dispatch_timer（on_timer、update_metrics、process_hedging、执行意图）
   - 策略或 Hedge 产生的订单经 append_order → order_executor（execute_order）：撮合、add_order、put_event(Order)、put_event(Trade)（含 combo 各 leg），EventEngine 再次同步 dispatch_order/dispatch_trade
   - 收集指标、调用 timestep_callbacks_
3. 汇总 BacktestResult（日序列、总 PnL、净 PnL、最大回撤、Sharpe 等），JSON 输出到 stdout。

**资源**：无外部网络或数据库；所有状态在进程内。

#### 5.1.0 回测入口参数与结果（entry_backtest）

- **必需参数**：第一类为 parquet 路径（单文件）或 `--files file1 file2 ...` 多文件；第二类为 strategy_name（策略类名）。
- **可选参数**：--fill-mode（mid|bid|ask）、--fee-rate、--slippage-bps、--risk-free-rate、--iv-price-mode、--log（启用日志）；以及 key=value 形式的策略参数。
- **环境变量**：BACKTEST_LOG=1 或 true 时开启日志级别为 INFO。
- **执行**：单文件时构建一个 BacktestEngine，多文件时可构建多个或复用 engine 并 reset；每个 engine 先 load_backtest_data、add_strategy、configure_execution，再 run()。
- **输出**：BacktestResult 汇总为 JSON 输出到 stdout，包含 status、strategy_name、portfolio_name、日序列（file、pnl、net_pnl、fees、orders、timesteps）、总 PnL、净 PnL、最大回撤、Sharpe 等；错误与进度可写 stderr。

**多文件与并行**：entry_backtest 支持 --files 后跟多个 parquet 路径，再跟 strategy_name。若采用多 engine 并行（如每文件一个 BacktestEngine），各 engine 独立 load_backtest_data、add_strategy、run，结果在入口层聚合为日序列或汇总指标。若复用同一 engine，需在每文件前 reset() 清空数据与策略状态，再重新 load_backtest_data、add_strategy、run。

#### 5.1.1 回测单步详细流程（单时间步）

1. BacktestEngine::run 内 iter_timesteps 回调得到 (ts, frame)。
2. put_event(Snapshot, get_precomputed_snapshot(step_count)) → EventEngine.put_event 同步执行：
   - dispatch_snapshot：main_engine_->get_portfolio(snap->portfolio_name)->apply_frame(*snap)，组合内价格与 Greeks 更新。
3. put_event(Timer) → EventEngine.put_event 同步执行：
   - dispatch_timer：  
     a. se->get_strategy()->on_timer()：策略读 portfolio、holding，可能调用 send_order/send_combo_order。  
     b. send_order 内部：RuntimeAPI.send_order → MainEngine.append_order → order_executor（即 execute_order）。  
     c. execute_order：生成 orderid、按 fill_mode 取价、撮合、main_engine_->add_order、put_event(Order)、put_event(Trade)。  
     d. put_event(Order) 再次进入 EventEngine：dispatch_order → position_engine->process_order、option_strategy_engine->process_order。  
     e. put_event(Trade)：dispatch_trade → position_engine->process_trade、option_strategy_engine->process_trade。  
     f. position_engine->update_metrics。  
     g. hedge_engine->process_hedging(..., out_orders, out_cancels, out_logs)；对结果逐条 send_order/cancel_order/put_log_intent。  
4. 本步结束后 BacktestEngine 从 strategy_engine->get_strategy_holding() 取 PnL、Delta 等，更新 current_pnl_、max_drawdown_ 等，调用 timestep_callbacks_。

### 5.2 实盘运行时（非 gRPC）

**初始化**

1. entry_live 安装 SIGINT/SIGTERM、load_dotenv。
2. 创建 EventEngine(interval)、MainEngine(&event_engine)；MainEngine 构造时创建各引擎、组装 RuntimeAPI、创建 OptionStrategyEngine、event_engine->start()（启动队列线程与定时器线程）、event_engine->set_main_engine(this)、db_engine_->load_contracts()。
3. load_contracts 从 PostgreSQL 读取合约，按固定顺序 put_event(Contract)；EventEngine 工作线程 process 时 dispatch_contract → MarketDataEngine::process_contract，建立 portfolios_ 与 option_apply_order_。

**连接与事件流**

1. main_engine.connect() → IbGateway::connect()，与 TWS 建立连接。
2. 定时器线程按间隔 put(Timer)；TWS 回报通过 IbGateway 回调 main_engine->put_event(Order/Trade/Contract)，入队。
3. 工作线程 run() 从队列取事件，process(event) 按类型 dispatch；Snapshot 事件由外部数据源或上游组件 put，用于更新组合价格与 Greeks。

**策略与订单**

1. 策略通过 gRPC 或后续扩展方式添加（AddStrategy）；OptionStrategyEngine::add_strategy 创建实例。
2. 策略在 on_timer 中通过 RuntimeAPI 发单；append_order → MainEngine::send_order → IbGateway::send_order。
3. 回报经 put_event(Order/Trade) 入队，dispatch_order/dispatch_trade 更新 OptionStrategyEngine 与 PositionEngine，并 save_order_data/save_trade_data。

**停止**：g_running 置 false 后 main_engine.disconnect()、main_engine.close()。

#### 5.2.1 实盘 MainEngine 构造与 load_contracts

1. MainEngine(&event_engine) 内：若未传 event_engine 则内部创建 EventEngine(1)；event_engine_ptr_->start() 启动队列线程与定时器线程。
2. 依次创建 LogEngine、PositionEngine、DatabaseEngine、MarketDataEngine、IbGateway。
3. 组装 RuntimeAPI（send_order→append_order，get_portfolio→get_portfolio 等），创建 OptionStrategyEngine(api)，event_engine_ptr_->set_main_engine(this)。
4. db_engine_->load_contracts()：从 PostgreSQL 读合约，按序对每条 put_event(EventType::Contract, ContractData)。事件入队后由工作线程 process → dispatch_contract → MarketDataEngine::process_contract，建立 portfolios_ 与 option_apply_order_。此时组合结构已就绪，价格与 Greeks 仍为初始值，待 Snapshot 事件更新。

#### 5.2.2 Live 启动后首次 Timer 前的状态

- MainEngine 构造完成时：EventEngine 已 start（队列线程与定时器线程运行）；load_contracts 已执行完毕，Contract 事件已被工作线程消费，MarketDataEngine 已建立 portfolios_ 与 option_apply_order_。
- 此时组合结构完整，但各 PortfolioData 内价格与 Greeks 仍为初始值（如 0）；尚未有 Snapshot 事件注入。若需策略在首帧就有行情，需在 connect 或后续由外部先 put_event(Snapshot, ...)。
- 定时器线程按 interval 秒投递 Timer；首个 Timer 到达后 dispatch_timer 会执行（ib_gateway process_timer_event、position process_timer_event、每策略 process_hedging、option_strategy_engine->on_timer()）。若尚未添加策略，on_timer 可能为空遍历。

#### 5.2.3 实盘事件来源

- **Timer**：定时器线程按 interval 秒 put(Event(Timer))。
- **Order / Trade**：IbGateway 在 TWS 回报回调中调用 main_engine->put_event(Order/Trade)。
- **Contract**：仅启动时 load_contracts 发出；后续若有合约变更可再 put_event(Contract)。
- **Snapshot**：由外部行情处理或上游服务组装 PortfolioSnapshot 后 put_event(Snapshot)；若实盘暂未接行情，可暂无 Snapshot，策略仅能依赖 Contract 阶段的结构与后续接入的 Snapshot。

### 5.3 实盘 gRPC 运行时

- entry_live_grpc 与 entry_live 类似创建 EventEngine 与 MainEngine，并构造 GrpcLiveEngineService(&main_engine)，gRPC Server 监听 0.0.0.0:50051，RegisterService(&service)，BuildAndStart() 后 server->Wait() 阻塞。
- 后端通过 EngineService 调用 GetStatus、ListStrategies、ConnectGateway、DisconnectGateway、StartMarketData、StopMarketData、AddStrategy、InitStrategy、RemoveStrategy、GetOrdersAndTrades、StreamLogs、StreamStrategyUpdates 等；各 RPC 内部直接调用 main_engine 的对应方法。
- 进程终止前 disconnect、close。

#### 5.3.1 gRPC 服务接口（EngineService）概览

GrpcLiveEngineService 实现 proto 定义的 EngineService，各 RPC 直接调用 MainEngine 或 OptionStrategyEngine 的对应能力：

| RPC | 职责 |
|-----|------|
| GetStatus | 引擎运行状态、是否连接 IB 等 |
| ListStrategies | 当前加载策略列表（名、类、组合、状态） |
| ConnectGateway / DisconnectGateway | 连接/断开 IbGateway |
| StartMarketData / StopMarketData | 启停行情更新（MarketDataEngine） |
| StartStrategy / StopStrategy | 启停指定策略 |
| StreamLogs | 流式输出日志（从 MainEngine 日志队列消费） |
| StreamStrategyUpdates | 流式输出策略更新（从 MainEngine strategy_updates_ 消费） |
| GetOrdersAndTrades | 当前订单与成交列表 |
| ListPortfolios / GetPortfoliosMeta | 组合列表与元信息 |
| ListStrategyClasses | 已注册策略类名（StrategyRegistry） |
| GetRemovedStrategies | 已移除策略列表 |
| AddStrategy / RestoreStrategy / InitStrategy / RemoveStrategy / DeleteStrategy | 策略生命周期管理 |
| GetStrategyHoldings | 各策略持仓 JSON |

---

## Part 6. 数据流

### 6.1 合约与组合结构

**回测 parquet 与快照格式（概念）**

- 回测数据来自 parquet 文件；时间列默认 ts_recv，标的符号可从文件名推断或显式传入。
- BacktestDataEngine 在 load_parquet 时解析标的与期权符号，构建 PortfolioData 与 option_apply_order_；每帧对应 parquet 中按时间分组的一批行，build_snapshot_from_frame 将该帧的 bid/ask/last/delta/gamma/theta/vega/iv 等按 option_apply_order_ 顺序填入 PortfolioSnapshot 的向量。
- 预计算后 snapshots_.size() 等于时间步数；run 时每步取 get_precomputed_snapshot(step_count) 作为 Snapshot 事件，无需在运行时再解析 parquet。快照格式与 PortfolioData::apply_frame 的约定一致（underlying 字段 + option 向量顺序）。

**回测**

- 合约与组合在 BacktestDataEngine::load_parquet 时建立：从 parquet 解析标的与期权符号，create_portfolio_data、build_option_apply_index、precompute_snapshots；portfolio_data_ 注册到 MainEngine，合约注册到 MainEngine。组合结构在 load 后固定，行情由每步 Snapshot 的 apply_frame 更新。

**实盘**

- 合约来自 DatabaseEngine::load_contracts，按固定顺序（如先 equity 后 option）put_event(Contract)。MarketDataEngine::process_contract 维护 contracts_、portfolios_（add_option、set_underlying），每次 add_option 后 finalize_chains，使 option_apply_order_ 稳定，供 apply_frame 使用。后续 Snapshot 事件只更新价格与 Greeks，不改变结构。

### 6.2 行情与快照流

- **回测**：BacktestDataEngine 预计算每帧 PortfolioSnapshot（与 portfolio option_apply_order 一致）；run 时每步 put_event(Snapshot, get_precomputed_snapshot(step))，再 put_event(Timer)。dispatch_snapshot 内 get_portfolio(snap->portfolio_name)->apply_frame(*snap)。
- **实盘**：组合结构由 Contract 事件建立；行情/Greeks 由外部或上游组装 PortfolioSnapshot 后 put_event(Snapshot)；dispatch_snapshot 同样 get_portfolio(name)->apply_frame(snapshot)。MarketDataEngine 不直接产生 Tick/Greeks，只维护组合结构与 apply_frame 的兼容顺序。

### 6.3 订单与成交流

1. **策略产单**：策略 on_timer 等中调用 engine_->send_order（即 RuntimeAPI.send_order）→ MainEngine.append_order。
2. **回测执行**：append_order → order_executor（BacktestEngine::execute_order）：生成 orderid、撮合、add_order、put_event(Order)、put_event(Trade)；EventEngine 同步 dispatch_order/dispatch_trade，更新 PositionEngine 与 OptionStrategyEngine。
3. **实盘执行**：append_order → MainEngine::send_order → IbGateway::send_order；TWS 回报经 IbGateway 回调 put_event(Order)/put_event(Trade)，入队后 dispatch_order/dispatch_trade 更新状态并 save_order_data/save_trade_data。
4. **策略感知**：通过 OptionStrategyEngine 的 orders_/trades_ 与 get_strategy_holding 的持仓变化；策略不直接依赖 EventEngine。

### 6.4 合约 → 组合 → 快照 的演化

- **回测**：BacktestDataEngine::load_parquet 阶段从 parquet 解析标的与期权符号 → create_portfolio_data 创建 PortfolioData、add_option/set_underlying → build_option_apply_index、precompute_snapshots 生成与 option_apply_order 一致的快照序列。运行时无新增合约；每步仅 apply_frame(snapshot) 更新数值。
- **实盘**：DatabaseEngine::load_contracts 从 DB 读 ContractData → put_event(Contract) → MarketDataEngine::process_contract 中 get_or_create_portfolio、add_option、set_underlying、finalize_chains。组合结构在 load_contracts 完成后稳定；后续 Snapshot 事件仅更新价格与 Greeks。若未来支持运行中新增合约，可再 put_event(Contract)，由 process_contract 扩展结构。

### 6.5 回测与实盘数据流对比（概要）

| 阶段 | 回测 | 实盘 |
|------|------|------|
| 合约与组合结构 | BacktestDataEngine::load_parquet 内 create_portfolio_data、add_option、finalize_chains；注册到 MainEngine | load_contracts 发 Contract 事件 → MarketDataEngine::process_contract 建 portfolios_、finalize_chains |
| 行情与 Greeks | 每步 get_precomputed_snapshot(step) 作为 Snapshot 事件；dispatch_snapshot → apply_frame | 外部或上游组装 PortfolioSnapshot 后 put_event(Snapshot)；同样 dispatch_snapshot → apply_frame |
| 订单执行 | append_order → order_executor（execute_order）撮合 → add_order、put_event(Order)、put_event(Trade) | append_order → IbGateway::send_order；TWS 回报 → put_event(Order/Trade) |
| 订单/成交持久化 | 无 | dispatch_order/dispatch_trade 内 save_order_data/save_trade_data |
| 策略驱动 | 每步先 Snapshot 再 Timer；dispatch_timer 内 on_timer、update_metrics、process_hedging、执行意图 | 定时器线程 put(Timer)；外部回报 put(Order/Trade)；dispatch_timer 内先 ib_gateway、position、hedge，最后 on_timer |

### 6.6 策略更新与监控流（Live）

- 策略通过 RuntimeAPI.put_strategy_event(StrategyUpdateData) 提交更新；MainEngine::on_strategy_event 将数据推入 strategy_updates_ 队列。
- gRPC StreamStrategyUpdates 在循环中调用 main_engine->pop_strategy_update，将 StrategyUpdateData 转为 proto StrategyUpdate 写入流，供后端或前端展示。
- 日志流：MainEngine 可将 LogData 写入 log_stream_buffer_，StreamLogs 从该队列消费并推送给客户端。

### 6.7 回测与实盘数据流时序（概念）

**回测**：  
T0 load_backtest_data → 建立组合、预计算快照；T1 add_strategy → 创建策略并绑定 "backtest" 组合；T2 run() 开始 → 每步：先 put_event(Snapshot) 更新组合数值 → put_event(Timer) → dispatch_timer 内 on_timer 可能 send_order → execute_order 撮合 → put_event(Order)、put_event(Trade) → dispatch_order、dispatch_trade 更新状态 → 下一步。无外部时钟，时间由 iter_timesteps 的帧顺序决定。

**Live**：  
T0 MainEngine 构造 → load_contracts 发 Contract → 工作线程 process 后 dispatch_contract 建组合；T1 connect() 连 TWS；T2 定时器线程周期性 put(Timer)，外部回报 put(Order/Trade)；工作线程依次 process(Snapshot)、process(Timer)、process(Order)、process(Trade)。策略通过 AddStrategy 添加后，在 dispatch_timer 的 on_timer() 中被驱动；行情由外部 put(Snapshot) 注入，无固定「每步」概念，取决于 Timer 间隔与 Snapshot 到达频率。

---

## Part 7. 扩展点

### 7.1 新增策略

1. 在 strategy/ 下新增派生 OptionStrategyTemplate 的类，实现 on_init_logic、on_timer_logic、on_stop_logic 等。
2. 在 strategy_registry.cpp 中 include 新头文件，并添加 REGISTER_STRATEGY(YourStrategyClassName)。
3. 回测在 entry_backtest 命令行传入策略类名；实盘通过 gRPC AddStrategy 传入类名与参数。

### 7.2 数据源

- **回测**：BacktestDataEngine 可扩展支持更多 parquet 格式或频率；快照格式保持 PortfolioSnapshot 与 option_apply_order 一致即可。
- **实盘**：只要能组装 PortfolioSnapshot 并按组合名 put_event(Snapshot)，即可对接不同行情源；MarketDataEngine 无需改逻辑。

### 7.3 风控与监控

- 可在 MainEngine 或单独 RiskEngine 中在 append_order 前做限额、仓位检查；或在对冲逻辑中扩展 HedgeEngine 参数。
- gRPC 已暴露 StreamLogs、StreamStrategyUpdates、GetStrategyHoldings 等，可用于前端或监控；可扩展更多 RPC 或过滤条件。

### 7.4 多组合与多账户

- PortfolioData 按 name 区分；StrategyHolding 按 strategy_name 区分。多账户可在 ContractData 或配置中扩展 account 等字段，IbGateway 与 DatabaseEngine 按需扩展。

### 7.5 新增入口或运行模式

- 若需新的可执行程序（如仅行情订阅、仅风控等），可参考 entry_live 或 entry_live_grpc：创建 EventEngine 与 MainEngine，按需调用 connect、load_contracts、add_strategy 等；不必须启动 gRPC。
- 若需回测多文件并行，entry_backtest 已支持 --files 多文件；每文件可对应独立 BacktestEngine 实例或复用 engine 后 reset。

### 7.6 替换基础设施实现

- **网关**：IbGateway 内部通过 IbApi 抽象与 IbApiTws 实现解耦；可增加其他 IbApi 实现或其它经纪商网关，只要在回报时调用 main_engine->put_event(Order/Trade/Contract) 即可。
- **数据库**：DatabaseEngine 当前为 PostgreSQL；若更换存储，需实现 load_contracts（按序发 Contract）、save_order_data、save_trade_data 等语义。
- **行情**：实盘行情只要产出 PortfolioSnapshot 并按组合名 put_event(Snapshot, snapshot)，无需改 MarketDataEngine 或策略逻辑。

### 7.7 扩展时的注意事项

- **新增策略**：必须实现 OptionStrategyTemplate 的 on_init_logic、on_timer_logic、on_stop_logic；在 strategy_registry.cpp 中 REGISTER_STRATEGY；回测使用固定 portfolio 名 "backtest"，live 由 AddStrategy 指定 portfolio_name。
- **新增事件类型**：需在 EventType 与 EventPayload 中增加；在 EventEngine 的 process/put_event 分支中增加对应 dispatch_*；若有新消费者，在 dispatch 顺序中明确调用顺序。
- **新增 Runtime 能力**：若 MainEngine 需暴露新接口（如风控开关），可在 MainEngine 与 GrpcLiveEngineService 中增加方法；Core 层仍不直接依赖这些能力，仅通过 RuntimeAPI 已定义的 get_*、send_order 等与 Runtime 交互。
- **多文件回测**：entry_backtest 的 --files 模式可多文件；每文件可对应独立 BacktestEngine 或复用同一 engine 并 reset()；reset 需清空数据、策略与指标但保留引擎实例，以便迭代下一文件。

---

## Part 8. 小结

- **Domain Core**：纯逻辑、无 Context；OptionStrategyEngine 通过 RuntimeAPI 与策略解耦；Position/Hedge/ComboBuilder 均为 caller 传参或产出 Intent，由 Runtime 执行。
- **Runtime**：Backtest 与 Live 差异集中在 EventEngine（同步 vs 队列+线程）、MainEngine（有无网关/数据库）、以及数据来源（预计算快照 vs Contract+Snapshot）。事件分发顺序写死，无 handler 注册。
- **Infrastructure**：回测数据、实盘行情/组合、数据库、网关各司其职；合约与组合结构的建立方式不同（load 时建 vs Contract 事件建），但 Snapshot 应用路径一致（apply_frame）。
- **事件与意图**：Event 入（Timer/Snapshot/Order/Trade/Contract），Intent 出（OrderRequest、CancelRequest、LogData）；Log 不占事件类型，统一经 put_log_intent → LogEngine。

本架构使回测与实盘共享同一套领域核心与策略实现，仅通过 Runtime 与 Infrastructure 切换数据与执行环境，便于维护与扩展。

### 8.1 架构要点速查

| 维度 | 要点 |
|------|------|
| 分层 | Domain Core（纯逻辑）→ Runtime（调度与执行）→ Infrastructure（数据、持久化、网关）→ Utilities（数据与抽象） |
| 事件 | Event 入：Timer、Snapshot、Order、Trade、Contract；无独立 Log 事件 |
| 意图 | Intent 出：OrderRequest、CancelRequest、LogData；执行由 Runtime 完成 |
| 策略环境 | OptionStrategyEngine 仅持 RuntimeAPI；策略通过 API 读组合/持仓、发单、写日志 |
| 分发 | EventEngine 按类型写死顺序 dispatch；无动态 handler 注册 |
| 回测 | BacktestEngine 驱动；每步 Snapshot → Timer；order_executor 撮合后 put_event(Order/Trade) 回灌 |
| 实盘 | 队列+定时器线程；load_contracts 发 Contract 建组合；Snapshot 更行情；IbGateway 回报 put_event |
| 日志 | put_log_intent → LogEngine；level 统一控制；单一 sink |


### 8.3 架构层面的风险与限制

- **事件顺序**：dispatch 顺序写死，若新增事件类型或需调整顺序，需改 EventEngine 实现；多策略 Live 下 dispatch_timer 内先 hedge 再 on_timer，策略看到的订单/成交状态已包含对冲结果。
- **单工作线程**：Live EventEngine 单线程消费事件队列；若某次 dispatch 耗时过长，会阻塞后续事件。Timer 与外部回报共享同一队列，需避免在 on_timer 或 process_contract 中做重计算。
- **组合结构一致性**：apply_frame 依赖 option_apply_order_ 与 Snapshot 向量顺序一致；回测在 load 时确定，Live 在 load_contracts 后 process_contract 确定。若 Contract 事件顺序或组合拓扑与 Snapshot 生产者不一致，会导致错位。
- **策略与 Runtime 契约**：策略仅通过 RuntimeAPI 访问环境；若 Runtime 未正确注入 get_portfolio、get_contract 等（如组合名错误、未 load_contracts），策略可能得到空指针或错误数据，需在调用方保证。
- **回测与实盘差异**：回测为同步、无网络、无持久化；实盘为异步队列、有网关与 DB。策略逻辑一致，但执行延迟、滑点、部分成交等仅在实盘存在，回测默认即时全成。

---

## 附录 A. 事件与意图速查表

### A.1 Event 类型与 Payload

| EventType | Payload 类型 | 典型生产者 | 典型消费者 |
|-----------|--------------|------------|------------|
| Timer | （无或 monostate） | 回测 run 每步；Live 定时器线程 | dispatch_timer |
| Snapshot | PortfolioSnapshot | BacktestDataEngine；Live 外部行情 | dispatch_snapshot → apply_frame |
| Order | OrderData | execute_order 后；IbGateway 回报 | dispatch_order |
| Trade | TradeData | execute_order 后；IbGateway 回报 | dispatch_trade |
| Contract | ContractData | load_contracts；IbGateway（若推送） | dispatch_contract → process_contract |

### A.2 Intent 提交路径

| Intent | 提交方式 | 执行方 |
|--------|----------|--------|
| 下单 | api_.send_order(strategy_name, req) → append_order → send_order | 回测 order_executor；Live IbGateway |
| 撤单 | append_cancel(req) → cancel_order | 回测更新状态+put_event(Order)；Live IbGateway |
| 日志 | api_.write_log(log) 或 put_log_intent(log) | LogEngine.process_log_intent |

### A.3 回测与 Live dispatch 顺序对照

- **Snapshot**：一致；get_portfolio(name)->apply_frame(snapshot)。
- **Timer**：回测顺序为 on_timer → update_metrics → process_hedging → 执行 orders/cancels/logs；Live 为 ib_gateway process_timer_event → position process_timer_event + put_log_intent → 每策略 process_hedging → 执行 → on_timer。
- **Order**：一致；position_engine->process_order；option_strategy_engine->process_order；Live 额外 save_order_data。
- **Trade**：回测先 position 后 option_strategy_engine；Live 先 option_strategy_engine、save_trade_data，再 position->process_trade。
- **Contract**：仅 Live；dispatch_contract → market_data_engine->process_contract。

---

## 附录 B. 逻辑闭环（文字描述）

**回测单步闭环**：  
iter_timesteps 每步 → put_event(Snapshot, get_precomputed_snapshot(step)) → put_event(Timer) → EventEngine 同步 process → dispatch_snapshot → get_portfolio()->apply_frame(snap) → dispatch_timer → on_timer / update_metrics / process_hedging → 产生 intents → main_engine_->send_order/cancel_order/put_log_intent → 若为订单则 order_executor 撮合 → main_engine_->add_order、put_event(Order)、put_event(Trade) → event_engine_->put_event 再次进入 → dispatch_order / dispatch_trade 更新状态。

**Live 事件闭环**：  
定时器或外部 → put(Event) 入队 → run() 取事件 process → dispatch_snapshot / dispatch_timer / dispatch_order / dispatch_trade / dispatch_contract。Snapshot 更新 portfolio；Contract 建立/更新 portfolio（process_contract + finalize_chains）。position/hedge 在 dispatch_timer 中产出 intents → send_order/cancel_order/put_log_intent → Gateway 执行。回报经 main_engine->put_event → event_engine->put(e) 入队 → 下一轮 process 更新状态；dispatch_order/dispatch_trade 内 save_order_data/save_trade_data。

策略与 Hedge 仅通过 RuntimeAPI 读入参、产出 orders/cancels/logs；执行与回灌均由 MainEngine（append_*、put_event）完成，不直接依赖 IEventEngine。

---

## 附录 D. 主要调用关系（谁调用谁）

以下为架构层面的典型调用方向，便于理解数据与控制流。

- **entry_backtest** → BacktestEngine（构造、load_backtest_data、add_strategy、configure_execution、run）。
- **BacktestEngine** → EventEngine.put_event(Snapshot/Timer)；MainEngine.load_backtest_data、get_portfolio、option_strategy_engine、set_order_executor；order_executor 即 BacktestEngine::execute_order。
- **BacktestEngine::execute_order** → MainEngine.add_order、put_event(Order)、put_event(Trade)。
- **EventEngine（回测）** → MainEngine.get_portfolio、option_strategy_engine、position_engine、hedge_engine、send_order、cancel_order、put_log_intent；dispatch_timer 内 get_strategy()->on_timer()、update_metrics、process_hedging。
- **OptionStrategyEngine** → 仅通过 RuntimeAPI 调用 MainEngine 能力（send_order、get_portfolio 等）；内部被 EventEngine 调用 process_order、process_trade、on_timer。
- **策略** → engine_->send_order、get_portfolio、get_holding、write_log、get_combo_builder_engine、get_hedge_engine 等（均来自 RuntimeAPI）。
- **entry_live** → EventEngine、MainEngine、main_engine.connect()、主循环、disconnect、close。
- **MainEngine（live）** → DatabaseEngine.load_contracts；EventEngine.set_main_engine；各引擎创建与 RuntimeAPI 组装。
- **load_contracts** → put_event(Contract) → EventEngine 入队 → process → dispatch_contract → MarketDataEngine.process_contract。
- **IbGateway** → main_engine->put_event(Order/Trade/Contract) 回报。
- **GrpcLiveEngineService** → main_engine 的 get_portfolio、option_strategy_engine、connect、send_order、add_strategy、pop_strategy_update 等。

---

## 附录 E. Proto 消息与服务摘要

- **EngineStatus**：running、connected、detail 等，GetStatus 响应。
- **StrategySummary**：strategy_name、class_name、portfolio、status，ListStrategies 流式条目。
- **Empty**：无参请求占位。
- **LogLine**：StreamLogs 流式条目。
- **StrategyUpdate**：strategy_name、class_name、portfolio、json_payload，StreamStrategyUpdates 流式条目。
- **AddStrategyRequest**：strategy_class、portfolio_name、setting_json 等；AddStrategy 请求。
- **OrdersAndTradesResponse**：当前订单与成交列表。
- **ListPortfoliosResponse**：组合名称或元信息列表。
- **ListStrategyClassesResponse**：已注册策略类名列表。
- **GetRemovedStrategiesResponse**：已移除策略列表。
- **StrategyHoldingsResponse**：各策略持仓 JSON。
- **EngineService**：定义 GetStatus、ListStrategies、ConnectGateway、DisconnectGateway、StartMarketData、StopMarketData、StartStrategy、StopStrategy、StreamLogs、StreamStrategyUpdates、GetOrdersAndTrades、ListPortfolios、GetPortfoliosMeta、ListStrategyClasses、GetRemovedStrategies、AddStrategy、RestoreStrategy、InitStrategy、RemoveStrategy、DeleteStrategy、GetStrategyHoldings 等 RPC。

具体字段与编号以 proto 文件为准。

---

## 附录 H. 设计决策与取舍（架构层面）

- **为何 Core 不持 IEventEngine**：Core 与策略只应产出「意图」（订单、撤单、日志），由 Runtime 决定何时、以何种方式执行。若 Core 持 IEventEngine，则与「Event 入、Intent 出」的边界模糊，且回测与 Live 的事件模型不同（同步 vs 队列），Core 难以抽象统一。
- **为何事件分发顺序写死**：可预测、易调试、无隐藏依赖；扩展新事件类型时需改 EventEngine 代码，但换来的是一目了然的调用链。若未来需要可配置顺序，可在 dispatch 内引入「阶段列表」配置，而不必改为动态 handler 注册。
- **为何 Log 不作为 Event 类型**：日志是「侧效应」，不需要参与事件驱动的状态机；统一为 Intent 经 put_log_intent 进入 LogEngine，由 level 统一过滤，避免事件流被大量日志稀释，且便于关闭或重定向日志而不影响 Event 队列。
- **为何回测与 Live 的 dispatch_timer 顺序不同**：回测单策略、无网关，先 on_timer 再 update_metrics 再 hedge 再执行意图；Live 需先让 IbGateway 消费 TWS 消息（process_timer_event），再更新持仓指标与对冲，最后统一 on_timer，避免策略在尚未收到最新回报时做决策。
- **为何 Snapshot 与 Contract 分离**：Contract 建立组合结构（拓扑），Snapshot 只更新数值；这样回测可预计算快照、Live 可由不同数据源组装 Snapshot，而不必改组合结构逻辑。apply_frame 假设 option_apply_order_ 已定，与 Contract 建立顺序一致。
- **为何 OptionStrategyEngine 持 RuntimeAPI 而非 MainEngine***：若持 MainEngine*，Core 将依赖 Runtime 的具体类型；持 RuntimeAPI（函数与访问器）则 Core 仅依赖抽象能力，便于单测与替换 Runtime 实现。

---

## 附录 I. 常见问题与对照

- **如何新增一种策略？** 在 strategy/ 下实现派生 OptionStrategyTemplate 的类，实现 on_init_logic、on_timer_logic、on_stop_logic；在 strategy_registry.cpp 中 include 并 REGISTER_STRATEGY(ClassName)。回测在命令行传入该类名；Live 通过 gRPC AddStrategy 传入 class 名与 portfolio_name、setting_json。
- **回测与实盘是否共用同一策略代码？** 是。OptionStrategyEngine 与策略实现均在 core/、strategy/，回测与 Live 仅 MainEngine 与 EventEngine、Infra 不同；策略通过 RuntimeAPI 访问环境，不感知回测或 Live。
- **组合结构从哪里来？** 回测：BacktestDataEngine::load_parquet 内 create_portfolio_data、add_option、finalize_chains，并注册到 MainEngine。Live：DatabaseEngine::load_contracts 发 Contract 事件 → MarketDataEngine::process_contract 建 portfolios_、finalize_chains。
- **行情/Greeks 如何进入组合？** 统一通过 Snapshot 事件：dispatch_snapshot → get_portfolio(name)->apply_frame(snapshot)。回测每步用 get_precomputed_snapshot(step)；Live 由外部或上游组装 PortfolioSnapshot 后 put_event(Snapshot)。
- **订单从策略到成交的路径？** 策略调用 engine_->send_order（即 RuntimeAPI.send_order）→ MainEngine.append_order → 回测为 order_executor 撮合后 put_event(Order)、put_event(Trade)；Live 为 IbGateway::send_order，TWS 回报后 put_event(Order)、put_event(Trade)。EventEngine 的 dispatch_order/dispatch_trade 更新 OptionStrategyEngine 与 PositionEngine；Live 并 save_order_data/save_trade_data。
- **为何 Core 不持 IEventEngine？** Core 只应产出 Intent，由 Runtime 执行；事件入口与顺序由 EventEngine 控制，Core 不订阅事件，仅在 dispatch 时被调用。
- **如何关闭回测日志？** MainEngine::set_log_level(engines::DISABLED) 或默认 LogEngine 已为 DISABLED；通过 BACKTEST_LOG 环境变量或入口参数 --log 可开启 INFO。
- **gRPC 与无 gRPC 实盘的区别？** 同一套 Live MainEngine 与 EventEngine；entry_live 仅启动引擎与 connect，无 RPC；entry_live_grpc 在此基础上启动 gRPC Server，将 MainEngine 能力暴露为 EngineService，供后端调用。

---

（文档完。若与代码实现有出入，以代码为准；本文档仅描述架构设计意图与职责划分。）
