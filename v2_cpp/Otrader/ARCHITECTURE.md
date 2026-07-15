# Otrader 架构设计文档

---

## 0. 文件树

```
Otrader/
├── entry_backtest.cpp                  # 回测可执行入口
├── entry_live_grpc.cpp                 # 实盘 + gRPC 服务入口（唯一 Live 入口）
│
├── runtime/                            # 运行时：回测与实盘差异集中在此
│   ├── backtest/
│   │   ├── engine_backtest.{cpp,hpp}   # 回测顶层控制器
│   │   ├── engine_event.{cpp,hpp}     # 回测事件引擎（同步分发）
│   │   └── engine_main.{cpp,hpp}      # 回测 MainEngine
│   │
│   └── live/
│       ├── engine_event.{cpp,hpp}      # 实盘事件引擎（队列 + 工作线程）
│       ├── engine_main.{cpp,hpp}       # 实盘 MainEngine
│       └── engine_grpc.{cpp,hpp}       # gRPC 服务实现
│
├── infra/                             # 基础设施：数据、持久化、网关
│   ├── marketdata/
│   │   ├── engine_data_historical.{cpp,hpp}  # 回测数据引擎（parquet → 快照）
│   │   └── engine_data_tradier.{cpp,hpp}     # 实盘行情/组合引擎
│   ├── db/
│   │   └── engine_db_pg.{cpp,hpp}     # PostgreSQL 合约/订单/成交
│   └── gateway/
│       └── engine_gateway_ib.{cpp,hpp} # IB TWS 网关
│
├── core/                               # 领域核心：策略引擎与辅助引擎
│   ├── engine_execution.{cpp,hpp}      # 订单/成交缓存与发单入口
│   ├── engine_option_strategy.{cpp,hpp} # 统一策略引擎 + RuntimeAPI
│   ├── engine_position.{cpp,hpp}       # 策略持仓管理
│   ├── engine_hedge.{cpp,hpp}          # 对冲引擎
│   ├── engine_combo_builder.{cpp,hpp}  # 组合腿构造
│   └── engine_log.{cpp,hpp}            # 日志引擎
│
├── strategy/                           # 策略实现与注册
│   ├── factory/                        # 具体策略实现
│   │   ├── straddletest.{cpp,hpp}
│   │   ├── iv_mean_revert.{cpp,hpp}
│   │   └── ...
│   ├── template.{cpp,hpp}              # 策略模板基类
│   └── strategy_registry.{cpp,hpp}     # 策略类名 → 工厂
│
└── utilities/                          # 通用数据模型与抽象
    ├── event.hpp                       # Event、EventType、EventPayload
    ├── object.hpp                      # OrderData、TradeData、ContractData、PortfolioSnapshot
    ├── portfolio.hpp                   # PortfolioData
    ├── base_engine.hpp                 # MainEngine 虚接口、BaseEngine 基类
    └── constant.hpp 等                 # 枚举与常量
```

---

## 1. 系统概览与核心概念

Otrader 是面向 **期权组合交易** 的 C++20 交易引擎。回测与实盘共用同一套领域核心（Core + Strategy），差异仅体现在运行时：数据来源、时钟驱动、订单执行与持久化。

### 1.1 功能定位与边界

| 模式 | 功能 | 入口 | 边界 |
|------|------|------|------|
| **回测** | 单进程历史数据回放、策略执行、订单撮合与绩效统计 | `entry_backtest` | 命令行启动，JSON 输出到 stdout；无网络或数据库依赖 |
| **实盘（gRPC）** | 行情来自 MarketDataEngine（如 Tradier）；通过 IB TWS 下单并记录成交，并通过 gRPC 对外暴露控制接口 | `entry_live_grpc` | 在 `0.0.0.0:50051` 暴露 EngineService；合约与订单/成交持久化到 PostgreSQL |

### 1.2 架构分层

| 层次 | 职责 | 主要组件 |
|------|------|----------|
| **Domain Core** | 纯逻辑：接收 Event、更新状态、输出 Intent；不直接下单、不读库、不访问网关 | OptionStrategyEngine、PositionEngine、HedgeEngine、ComboBuilderEngine、LogEngine、ExecutionEngine、Strategy 实现 |
| **Runtime** | 接入数据与时钟，执行 Core 产出的 Intent，将执行结果转为 Event 回灌 | BacktestEngine、EventEngine（backtest/live）、MainEngine（backtest/live）、gRPC Service |
| **Infrastructure** | 数据源、持久化、网关 | BacktestDataEngine、MarketDataEngine、DatabaseEngine、IbGateway |
| **Utilities** | 通用数据模型、事件与引擎抽象 | event.hpp、portfolio.hpp、object.hpp、base_engine.hpp、constant.hpp 等 |

### 1.3 引擎引用与互动关系

**所有权**：MainEngine 持有所有引擎实例（EventEngine、LogEngine、OptionStrategyEngine、PositionEngine、HedgeEngine、ComboBuilderEngine、ExecutionEngine，以及 Infrastructure 层的 DatabaseEngine、MarketDataEngine、IbGateway）。EventEngine 持有 MainEngine 的非拥有指针，通过 MainEngine 的 accessor 访问各引擎，自身不直接持有引擎实例。

**Event 流向**：外部生产者（BacktestEngine、MarketDataEngine、IbGateway）调用 MainEngine 的 `put_event`，MainEngine 转发给 EventEngine。EventEngine 按类型分发：Snapshot → 组合 `apply_frame`；Timer → OptionStrategyEngine（驱动策略、PositionEngine、HedgeEngine、ExecutionEngine）；Order/Trade → ExecutionEngine 与 PositionEngine 更新状态，再经 OptionStrategyEngine 回调策略的 `on_order`/`on_trade`。

**Intent 流向**：策略与 HedgeEngine 通过 RuntimeAPI（send_order、cancel_order、write_log）产出 Intent。RuntimeAPI 绑定到 MainEngine：订单/撤单 Intent 进入 EventEngine 的 `put_intent`（实盘）或 BacktestEngine 的撮合路径（回测）；日志 Intent 进入 LogEngine。OptionStrategyEngine 在构造时接收 RuntimeAPI；HedgeEngine 与 ComboBuilderEngine 在需要时通过 SystemAPI 获取。

**Core 隔离**：OptionStrategyEngine、PositionEngine、HedgeEngine、ComboBuilderEngine、LogEngine、ExecutionEngine 不持有 MainEngine 或 EventEngine，所需能力通过 RuntimeAPI 或 caller 传入的回调（如 `get_portfolio`、`send_impl`）获得。

### 1.4 Event 入、Intent 出

**原则**：输入为 Event，输出为 Intent。固定分发顺序：Snapshot → 更新组合；Timer → 策略、持仓、对冲、执行意图；Order/Trade → 更新状态。合约元数据通过 `load_contracts` 或 `register_contract` 在初始化阶段同步注入，不经过事件队列。策略与 Hedge 仅通过 RuntimeAPI 提交 Intent。

**Event** — 来源与驱动作用：

| 类型 | 含义 | 来源 | 驱动 |
|------|------|------|------|
| **Snapshot** | 组合在某一时刻的价格与 Greeks 快照 | 回测预计算、实盘行情注入 | 组合状态更新；为策略与风险引擎提供统一视图 |
| **Timer** | 时钟/周期驱动 | 回测每步、实盘定时器线程 | 策略、持仓、对冲与执行逻辑 |
| **Order** | 订单状态更新 | 回测撮合、IbGateway 回报 | 订单生命周期感知；持仓与策略状态更新 |
| **Trade** | 成交回报 | 回测撮合、IbGateway 回报 | 持仓、PnL 与风控指标更新 |

**Intent** — 在各 Runtime 中的执行方式：

| 类型 | 含义 | 回测 | 实盘 |
|------|------|------|------|
| **OrderRequest** | 下单请求 | 回测撮合逻辑在后续时间步中消费；订单/成交状态更新 | 经 IbGateway 发送；回报转为 Order/Trade 事件 |
| **CancelRequest** | 撤单请求 | 内部订单状态更新以控制撮合 | 经 IbGateway 发送；结果反映在订单状态 |
| **LogData** | 日志 | LogEngine 按 level 过滤；统一格式便于回放与分析 | 同样格式；侧重实时监控与排障 |

**Backtest vs Live — 事件循环形态**：

| 维度 | 回测 | 实盘 |
|------|------|------|
| 时间与事件来源 | 时间步按固定顺序生成 Snapshot / Order / Trade / Timer | 真实时钟 + 外部系统：定时器线程、行情、网关回调 |
| EventEngine | 单线程、同步；当步所有事件在同一上下文中消费 | 队列 + 工作线程；生产者入队，工作线程消费；独立定时器线程 |
| 侧重点 | 确定性、可重现、可回放、结果统计 | 实时响应、对外可见性、监控、容错 |

### 1.5 RuntimeAPI

MainEngine 注入给 OptionStrategyEngine 的能力集合；策略仅通过该 API 访问环境。Core 不持有 MainEngine 或 IEventEngine；所需能力由 caller 传参或 RuntimeAPI 注入。

| 分组 | 职责 |
|------|------|
| **ExecutionAPI** | 发单、撤单、订单/成交查询、活跃订单跟踪 |
| **PortfolioAPI** | 组合/合约视图、策略持仓生命周期 |
| **SystemAPI** | 日志、辅助引擎（对冲、组合构造）、策略事件推送 |

### 1.6 组合与快照

| 类型 | 说明 |
|------|------|
| **PortfolioData** | 组合顶层结构；`option_apply_order_` 固定期权指针顺序，与 Snapshot 向量一一对应 |
| **PortfolioSnapshot** | 紧凑快照；`apply_frame(snapshot)` 将价格与 Greeks 写回组合内各 OptionData 与 UnderlyingData |
| **StrategyHolding** | 每策略一份；内含标的持仓与期权持仓（单腿与组合统一在 optionPositions）及 PnL、Greeks 等汇总 |

### 1.7 可扩展性

OptionStrategyEngine 通过 RuntimeAPI 注入能力，与运行环境解耦。StrategyRegistry + REGISTER_STRATEGY 支持按类名创建策略实例。

---

## 2. 分层设计

---

### 2.1 Domain Core

纯业务逻辑：接收 Event（Timer、Snapshot、Order、Trade），更新内部状态，输出 Intent（下单、撤单、日志）。包含策略实现、持仓跟踪、对冲逻辑、组合构造、执行缓存与日志，均不直接下单、不访问数据库或网关。环境访问仅通过 RuntimeAPI 注入。

| 组件 | 职责 | 边界 |
|------|------|------|
| **OptionStrategyEngine** | 策略实例管理与生命周期调度（on_init / on_start / on_stop / on_timer）；将 RuntimeAPI 暴露给策略；处理 Order/Trade 事件并回调策略的 on_order / on_trade | 仅依赖 RuntimeAPI，不直接依赖 MainEngine 或 EventEngine |
| **PositionEngine** | 维护策略持仓（StrategyHolding）；根据 Order/Trade 更新持仓；按组合更新汇总指标（update_metrics） | caller 传入 get_portfolio、portfolio 等；无执行类回调 |
| **HedgeEngine** | 集中式 delta 等对冲逻辑；根据 portfolio、holding、活跃订单等只读参数产出 orders、cancels、logs | 纯只读输入；执行由 Runtime 对产出结果调用 send_order / cancel_order / put_log_intent |
| **ComboBuilderEngine** | 按 ComboType 与期权数据生成标准化 Leg 与组合签名 | 纯函数风格；get_contract 由 caller 传入 |
| **LogEngine** | 消费 LogIntent；按 level 过滤后输出到 sink | 单一 sink，由 LogEngine 的 level 统一控制 |
| **ExecutionEngine** | 集中缓存订单与成交；维护订单与策略映射；封装下单入口（接收策略名 + OrderRequest，调用 runtime 注入的 send_impl） | 策略与 MainEngine 通过 RuntimeAPI.execution 与之交互，不直接操作容器 |
| **Strategy 层** | 实现具体策略逻辑（派生 OptionStrategyTemplate）；在 on_timer_logic 等中读组合/持仓、产生产单/撤单/日志意图 | 仅通过 RuntimeAPI 访问环境；StrategyRegistry 维护类名 → 工厂 |

### 2.2 Runtime

编排事件循环：接入数据与时钟，按固定顺序向 Core 分发事件，执行 Intent（将订单/撤单发送至网关或回测撮合），并将执行结果（成交、状态更新）转回 Event 反馈给 Core。回测运行时同步驱动时间步；实盘运行时使用队列与工作线程，由外部行情与网关回调注入事件。

| 组件 | 职责 | 边界 |
|------|------|------|
| **MainEngine** | 持有各引擎实例；提供 send_order、cancel_order、put_log_intent、get_portfolio、get_contract、get_holding 等能力接口；组装 RuntimeAPI 并注入 OptionStrategyEngine | 不包含「按事件类型决定调用顺序」的逻辑；put_event 转发给 EventEngine |
| **EventEngine** | 接收事件；按事件类型与固定顺序分发（dispatch_snapshot、dispatch_timer、dispatch_order、dispatch_trade）；执行意图通过 MainEngine 完成 | 不持有引擎实例，通过 MainEngine 的 accessor 访问各引擎。回测为同步分发；实盘为队列 + 工作线程 + 定时器线程 |
| **BacktestEngine** | 回测顶层控制器；按时间步驱动 Snapshot → 撮合 → Timer；将 submit_order 注入 MainEngine 用于撮合 | 单线程同步；无外部网络或数据库 |
| **Live 实盘** | EventEngine 使用队列与定时器线程；MainEngine 持有 DatabaseEngine、MarketDataEngine、IbGateway；构造时 load_contracts 建立组合结构；append_order / append_cancel 转 IbGateway；dispatch_order / dispatch_trade 内 save_order_data / save_trade_data | 合约由 load_contracts 回调直接调用 market_data_engine_->process_option / process_underlying 建立，无 Contract 事件入队 |
| **gRPC Service** | 持有 MainEngine*；暴露 EngineService（GetStatus、ListStrategies、AddStrategy、StreamStrategyUpdates 等）；各 RPC 直接调用 MainEngine 或 OptionStrategyEngine 方法 | 仅包装已有能力，不新增领域逻辑 |

### 2.3 Infrastructure

数据源、持久化与网关。提供行情、合约/订单/成交存储与经纪商连接。

| 组件 | 职责 | 边界 |
|------|------|------|
| **BacktestDataEngine** | 从 parquet 加载历史数据；构建回测用 PortfolioData；预计算每帧 PortfolioSnapshot；提供 iter_timesteps 与 get_precomputed_snapshot | 回测组合结构在 load_parquet 时建立；运行时仅通过 apply_frame 更新价格与 Greeks |
| **MarketDataEngine** | 经 load_contracts 回调 process_option / process_underlying 建立 contracts_、portfolios_；finalize_all_chains 后 apply_frame 可用；行情由 inject_tradier_chain 构建 Snapshot 后 put_event 注入 | 不直接产生行情；行情由外部或 inject 构建 Snapshot 后注入 |
| **DatabaseEngine** | PostgreSQL；load_contracts 按固定顺序（先 option 后 equity）遍历，对每条 ContractData 调用 apply_option / apply_underlying 回调；save_order_data / save_trade_data 在 dispatch_order / dispatch_trade 中被调用 | load_contracts 不 put_event；回调直接建立组合结构 |
| **IbGateway** | 封装 IB TWS 连接；send_order / cancel_order 下发；订单/成交回报通过 main_engine->put_event(Order/Trade) 回灌 | process_timer_event 用于周期性消费 TWS 消息队列 |

---

### 2.4 Utilities

通用数据模型、事件类型与引擎基类。定义 Event/EventType/EventPayload、OrderData/TradeData/ContractData/PortfolioSnapshot/StrategyHolding、PortfolioData/ChainData/OptionData/UnderlyingData，以及 MainEngine/BaseEngine 接口。被 Core、Runtime 与 Infrastructure 共用。

| 类别 | 内容 |
|------|------|
| **数据与枚举** | constant.hpp、object.hpp（OrderRequest、OrderData、TradeData、ContractData、PortfolioSnapshot、StrategyHolding 等）、portfolio.hpp（PortfolioData、ChainData、OptionData、UnderlyingData） |
| **事件抽象** | event.hpp（EventType、EventPayload、Event） |
| **引擎抽象** | base_engine.hpp（MainEngine 虚接口、BaseEngine 基类） |
