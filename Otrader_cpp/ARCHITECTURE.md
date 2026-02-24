## Otrader 系统架构文档

### Part 0. 文件树

```
Otrader/
├── ARCHITECTURE.md
├── CMakeLists.txt
├── entry_backtest.cpp
├── entry_live.cpp
├── entry_live_grpc.cpp
├── runtime/
│   ├── backtest/
│   │   ├── CMakeLists.txt
│   │   ├── engine_backtest.cpp
│   │   ├── engine_backtest.hpp
│   │   ├── engine_event.cpp
│   │   ├── engine_event.hpp
│   │   ├── engine_main.cpp
│   │   └── engine_main.hpp
│   └── live/
│       ├── CMakeLists.txt
│       ├── engine_event.cpp
│       ├── engine_event.hpp
│       ├── engine_grpc.cpp
│       ├── engine_grpc.hpp
│       ├── engine_main.cpp
│       └── engine_main.hpp
├── infra/
│   ├── db/
│   │   ├── engine_db_pg.cpp
│   │   └── engine_db_pg.hpp
│   ├── gateway/
│   │   ├── engine_gateway_ib.cpp
│   │   └── engine_gateway_ib.hpp
│   └── marketdata/
│       ├── engine_data_historical.cpp
│       ├── engine_data_historical.hpp
│       ├── engine_data_tradier.cpp
│       └── engine_data_tradier.hpp
├── proto/
│   ├── otrader_engine.proto
│   └── (生成: otrader_engine.pb.{cc,h}, otrader_engine.grpc.pb.{cc,h})
├── core/
│   ├── CMakeLists.txt
│   ├── engine_combo_builder.cpp
│   ├── engine_combo_builder.hpp
│   ├── engine_hedge.cpp
│   ├── engine_hedge.hpp
│   ├── engine_log.cpp
│   ├── engine_log.hpp
│   ├── engine_option_strategy.cpp
│   ├── engine_option_strategy.hpp
│   ├── engine_position.cpp
│   ├── engine_position.hpp
│   └── log_sink.hpp
├── strategy/
│   ├── CMakeLists.txt
│   ├── high_frequency_momentum.cpp
│   ├── high_frequency_momentum.hpp
│   ├── strategy_registry.cpp
│   ├── strategy_registry.hpp
│   ├── template.cpp
│   └── template.hpp
├── utilities/
│   ├── CMakeLists.txt
│   ├── README.md
│   ├── base_engine.hpp
│   ├── constant.hpp
│   ├── event.hpp
│   ├── ib_mapping.hpp
│   ├── lets_be_rational_api.hpp
│   ├── object.cpp
│   ├── object.hpp
│   ├── occ_utils.cpp
│   ├── occ_utils.hpp
│   ├── parquet_loader.cpp
│   ├── parquet_loader.hpp
│   ├── portfolio.cpp
│   ├── portfolio.hpp
│   ├── types.hpp
│   ├── utility.cpp
│   └── utility.hpp
├── tests/
│   ├── CMakeLists.txt
│   ├── backtest/
│   │   ├── CMakeLists.txt
│   │   ├── test_backtest.cpp
│   │   ├── test_backtest_data.cpp
│   │   └── test_entry_multi.cpp
│   └── live/
│       ├── CMakeLists.txt
│       └── test_live_components.cpp
└── .vscode/
    └── 现状描述.md
```

（说明：`proto/` 下 `.pb.cc`、`.pb.h`、`.grpc.pb.cc`、`.grpc.pb.h` 由 `otrader_engine.proto` 经 protobuf/gRPC 工具链生成，一般不纳入版本控制的生成物可依项目约定从树中省略。）

---

### 1. 概览

#### 1.1 背景与目标

Otrader 是一个围绕期权组合交易场景构建的 C++20 交易引擎子系统，对应 Python 版 `utilities/`、`core/`、`backtest/`、`live/` 等模块的高性能实现。  
它承担两类核心职责：

- **离线回测引擎（Backtest）**：在单进程内完成数据回放、策略执行、订单撮合与绩效统计，对应可执行程序 `entry_backtest`。
- **实时实盘引擎（Live）**：通过 IB TWS 网关接收真实行情、下单并记录成交，同时暴露 gRPC 服务给上层后端，对应可执行程序 `entry_live` 和 `entry_live_grpc`。

整个 C++ 子系统围绕「**统一的期权组合定价与策略运行时（`core::OptionStrategyEngine`）**」展开构建，Backtest 与 Live 共用这套核心，分别在数据来源、订单执行与持久化层面做适配。

#### 1.2 核心设计理念

- **Python/C++ 双栈对齐**：`utilities_cpp` 与 Python `utilities` 一一对应，结构和命名尽量保持一致，便于跨语言迁移和验证。
- **统一策略引擎**：不区分 live/backtest 的策略接口，通过 `RuntimeAPI` 注入「发送订单、获取组合、获取合约、获取持仓」等能力，从而让策略逻辑与运行环境解耦。
- **事件驱动与组合视图**：用 `Event` 与 `PortfolioData` 描述整个市场与组合状态，所有行情/Greeks 更新都通过「快照帧（`PortfolioSnapshot`）」来驱动。
- **强类型数据模型**：使用大量结构体（`ContractData`、`OptionData`、`PortfolioData` 等）描述期权组合、持仓与订单，避免在关键路径上传递松散的字典型结构。
- **可扩展的策略注册机制**：通过 `StrategyRegistry` 与 `REGISTER_STRATEGY` 宏，对策略类进行集中注册，支持在 Backtest 和 Live 中统一创建。

#### 1.3 模块概览

Otrader 目录结构（仅核心模块）：

- `utilities/`：通用数据结构与基础设施（枚举常量、订单/合约/持仓结构体、组合视图、事件、基础引擎接口等）。
- `core/`：统一策略引擎与辅助引擎（`OptionStrategyEngine`、`HedgeEngine`、`ComboBuilderEngine`、日志 sink 等）。
- `strategy/`：具体策略实现与策略模板（如 `HighFrequencyMomentumStrategy`），通过注册表对接 `OptionStrategyEngine`。
- `runtime/backtest/`：回测引擎、数据加载与执行逻辑，包括 `BacktestEngine`、回测版 `MainEngine`、回测版 `EventEngine`；历史数据由 `infra/marketdata/engine_data_historical` 中的 `BacktestDataEngine` 提供。
- `runtime/live/`：实盘引擎运行时，包含 `MainEngine`、`EventEngine` 及 gRPC 包装层（`engine_grpc`）；实盘所用的行情引擎、数据库、网关分别位于 `infra/marketdata/`（`engine_data_tradier`）、`infra/db/`（`engine_db_pg`）、`infra/gateway/`（`engine_gateway_ib`）。
- `proto/`：面向后端的 gRPC 接口定义（`EngineService`），用于远程控制 live 引擎与拉取状态。
- `entry_backtest.cpp` / `entry_live.cpp` / `entry_live_grpc.cpp`：不同运行模式的入口。

后续章节将围绕这些模块，从「静态结构视图」「运行时视图」「数据与事件流」「扩展点设计」四个维度展开描述。

---

### 2. 静态结构视图

#### 2.1 Utilities 层：通用数据模型与基础设施

Utilities 层是整个系统的「语言层基石」，大部分模块只依赖 utilities 与 C++ 标准库。其职责包括：

- 定义所有基础枚举、常量与字符串转换函数（`constant.hpp`）。
- 封装订单、成交、合约、持仓等基础对象（`object.hpp`）。
- 定义期权组合视图与快照应用逻辑（`portfolio.hpp`）。
- 抽象事件系统最小接口与事件载体（`event.hpp`）。
- 提供运行时引擎抽象基类（`base_engine.hpp`）。

##### 2.1.1 常量与枚举（`utilities/constant.hpp`）

**关键职责**：

- 提供交易方向、订单状态、产品类型、期权类型、组合类型、交易所等枚举。
- 提供枚举到字符串的转换函数（`to_string` 系列）。
- 统一「订单是否仍在进行中」的判断逻辑（`is_active_status`）。

**主要枚举**：

- **`Direction`**：`LONG` / `SHORT` / `NET`，分别表示多头、空头及净头寸。
- **`Status`**：`SUBMITTING` / `NOTTRADED` / `PARTTRADED` / `ALLTRADED` / `CANCELLED` / `REJECTED`，贯穿订单生命周期。
- **`Product`**：涵盖股指、期货、期权、指数、ETF、债券、基金等。
- **`OrderType`**：目前为 `LIMIT` 与 `MARKET`。
- **`OptionType`**：`CALL` 与 `PUT`。
- **`ComboType`**：包含 `SPREAD`、`STRADDLE`、`STRANGLE`、`IRON_CONDOR` 等多种组合类型，反映系统对复杂期权结构的抽象能力。
- **`Exchange`**：`SMART`、`NYSE`、`NASDAQ`、`CBOE` 等，IB 相关交易所枚举。

这些枚举在 `object.hpp`、`portfolio.hpp`、策略引擎与网关层广泛使用，保证跨模块语义一致。

##### 2.1.2 基础对象模型（`utilities/object.hpp`）

该文件定义了 Otrader 的核心数据结构，主要包括：

- **日志与基础数据**：`BaseData`、`LogData`。
- **行情数据**：`TickData`、`OptionMarketData`、`ChainMarketData`、`PortfolioSnapshot`。
- **合约数据**：`ContractData`。
- **订单 & 成交**：`Leg`、`OrderRequest`、`OrderData`、`CancelRequest`、`TradeData`。
- **持仓与组合汇总**：`BasePosition` 及其派生 `OptionPositionData`、`UnderlyingPositionData`、`ComboPositionData`，以及 `PortfolioSummary`、`StrategyHolding`。

**合约模型（`ContractData`）要点**：

- 用于描述标的、期权合约与其元数据（合约代码、交易所、乘数、tick 步长）。
- 对期权扩展字段包括：行权价、对应标的、期权类型、上市/到期日、所属组合（`option_portfolio`）、链 index 等。
- 同时包含 `con_id`、`trading_class` 等 IB 特有字段，支撑与 TWS 的映射。

**订单与成交模型要点**：

- `OrderRequest` 表示从策略或者上层系统发出的下单意图。
- `OrderData` 表示在 OMS 视角下的订单状态，包含 `status`、`traded`、组合腿、引用（`reference`）等。
- `TradeData` 表示实际成交记录，与订单通过 `orderid` 关联。
- `Leg` 用于表示组合腿，包括 `con_id`、方向、比例、价格等，可选 `symbol` 与 `trading_class`。

**持仓与组合汇总要点**：

- `BasePosition` 记录头寸数量、成本、当前价格、Greeks 等。
- `OptionPositionData`、`UnderlyingPositionData` 和 `ComboPositionData` 细化不同资产类别的特性（如 multiplier）。
- `StrategyHolding` 聚合一个策略的标的持仓、期权持仓、组合持仓以及整体 `PortfolioSummary`。

这些对象构成了策略引擎与回测/实盘运行时之间的「共享语言」。

##### 2.1.3 组合与期权链视图（`utilities/portfolio.hpp`）

该模块提供了围绕「组合-标的-期权链-期权合约」的多层结构视图：

- **`OptionData`**：封装单个期权合约在组合中的视角。
- **`UnderlyingData`**：描述组合标的的价格与 Tick。
- **`ChainData`**：期权链视图，维护某一到期日或某一组期权的整体信息。
- **`PortfolioData`**：顶层组合视图，聚合所有期权、链和标的。

**关键点**：

- `OptionData` 关联 `PortfolioData`、`ChainData` 与 `UnderlyingData`，并附带 delta/gamma/theta/vega/iv 等希腊值。
- `ChainData` 按 index（行权价或某种编码）管理 call/put 集合，并计算 ATM 价格、days_to_expiry/time_to_expiry 以及 skew 等统计量。
- `PortfolioData` 负责：
  - 接受 `ChainMarketData` 与 `TickData` 更新组合。
  - 维护一个固定的 `option_apply_order_`，用于 `PortfolioSnapshot` 的紧凑帧应用。
  - 提供 `apply_frame` 方法，将外部行情快照快速写入组合结构。

这种设计使得:

- live 模式下，真实行情（含 Greeks）可以预处理为 `PortfolioSnapshot` 并通过事件应用。
- backtest 模式下，可以从 parquet 中解析出帧数据后，同样通过 `PortfolioSnapshot` 更新组合状态。

##### 2.1.4 事件系统基础（`utilities/event.hpp` 与 `utilities/portfolio.hpp` 中的 `IEventEngine`）

事件系统由两部分构成：

- **事件定义（`event.hpp`）**：
  - `EventType`：`Timer` / `Order` / `Trade` / `Contract` / `Snapshot`。
  - `EventPayload`：`std::variant` 包含 `OrderData`、`TradeData`、`ContractData`、`PortfolioSnapshot` 等。
  - `Event`：承载事件类型与 payload。
  - `StrategyUpdateData`：策略更新的结构化载体，用于 live gRPC 侧向外输出策略状态变化。

- **事件引擎抽象（`IEventEngine`）**：
  - 提供 `start/stop`、`register_handler`、`put_intent_send_order`、`put_intent_cancel_order`、`put_intent_log`、`put_event` 等接口。
  - 默认实现多为 no-op，具体逻辑在 backtest 与 live 的 EventEngine 中实现。

通过 `IEventEngine`，回测与实盘可以在「订单/成交/快照」的处理方式上有所区别，但对上层组合与策略保持统一接口。

##### 2.1.5 通用引擎基类（`utilities/base_engine.hpp`）

该文件提供了一个非常薄的引擎抽象：

- **`utilities::MainEngine`** 抽象：
  - 提供 `write_log` 与 `put_event` 两个方法，默认 no-op。
  - backtest 与 live 版本的 `MainEngine` 都会实现该接口。

- **`utilities::BaseEngine`**：
  - 作为所有功能引擎（如 `MarketDataEngine`、`DatabaseEngine`）的基类，持有一个 `MainEngine*` 与自身名称。
  - 提供统一的构造与 `close` 钩子，便于资源释放与调试。

Utilities 层的这些抽象为上层核心与运行时提供了统一的语义基石。

---

#### 2.2 Core 层：统一策略引擎与核心辅助引擎

Core 层以 `OptionStrategyEngine` 为中心，将策略实例、订单状态与 runtime API 粘合在一起，形成一个对 backtest/live 透明的统一策略运行环境。

##### 2.2.1 RuntimeAPI：策略引擎与运行时解耦

`core/engine_option_strategy.hpp` 定义了 `RuntimeAPI` 结构体，它由 runtime（`MainEngine`）构造并注入 `OptionStrategyEngine`：

- **订单相关**：
  - `send_order(strategy_name, OrderRequest)`：发送普通订单。
  - `send_combo_order(strategy_name, ComboType, combo_sig, Direction, price, volume, legs, OrderType)`：发送组合订单。

- **环境读取**：
  - `get_portfolio(portfolio_name)`：获取组合视图。
  - `get_contract(symbol)`：获取合约元数据。
  - `get_holding(strategy_name)` / `get_or_create_holding(strategy_name)`：访问或创建策略持仓。

- **辅助功能**：
  - `write_log(LogData)`：写入日志。
  - `get_combo_builder_engine()` / `get_hedge_engine()`：访问核心辅助引擎实例。
  - `put_strategy_event(StrategyUpdateData)`：向外部（如 gRPC 流）报告策略更新事件。

通过 RuntimeAPI，策略引擎不直接依赖回测或实盘环境，所有环境差异都在 `MainEngine` 层实现。

##### 2.2.2 OptionStrategyEngine：策略运行与 OMS 状态

`OptionStrategyEngine` 负责：

- 管理策略实例（`strategy_cpp::OptionStrategyTemplate` 的派生类）。
- 跟踪订单与成交状态（维护 `orders_`、`trades_`、`strategy_active_orders_` 等）。
- 将 runtime API 暴露给策略，并通过策略生命周期钩子（`on_init`/`on_start`/`on_stop`/`on_timer` 等）调度策略逻辑。

**核心职责**：

- **策略管理**：
  - `add_strategy(class_name, portfolio_name, setting)`：创建并注册策略实例。
  - `init_strategy` / `start_strategy` / `stop_strategy` / `remove_strategy`。
  - 提供基于策略名的持仓访问与组合访问。

- **订单与成交流转**：
  - `process_order` / `process_trade`：更新内部 OMS 状态 + 更新策略持仓。
  - `get_order` / `get_trade` / `get_all_orders` / `get_all_trades` / `get_all_active_orders`。
  - `get_strategy_name_for_order(orderid)`：用于在 live 中将订单/成交与策略绑定（比如写入 DB 时打上策略名）。

- **订单下发接口**：
  - 提供多种 overload 的 `send_order` 与 `send_combo_order`，包括传入 `symbol`/`price`/`volume` 简化接口。

通过 OptionStrategyEngine，策略层可以在不关心底层撮合与连接的情况下，专注于「组合状态 → 交易信号 → 订单请求」的闭环。

##### 2.2.3 辅助引擎：头寸管理、组合构造与对冲

Core 层还包含：

- `engine_position`：管理 `StrategyHolding`，为策略与 runtime 提供持仓读写接口。
- `engine_combo_builder`：根据 `ComboType` 和 Legs 生成标准化组合结构。
- `engine_hedge`：为策略或 runtime 提供对冲策略（如 delta 对冲、gamma 对冲）的执行器。
- `engine_log` 与 `log_sink`：统一日志收集与输出（可接入不同 sink，如控制台、文件或数据库）。

这些引擎通过 RuntimeAPI 间接暴露给策略，形成一个对策略透明的「组合交易工具箱」。

---

#### 2.3 Strategy 层：策略模板与具体策略实现

Strategy 层主要包含：

- `strategy/template.hpp`：通用期权策略模板基类 `OptionStrategyTemplate`。
- `strategy/high_frequency_momentum.hpp/cpp`：高频动量策略实现。
- `strategy/strategy_registry.hpp/cpp`：策略注册表与工厂。

##### 2.3.1 策略注册表（`strategy/strategy_registry.hpp`）

`StrategyRegistry` 维护从策略类名到工厂函数的映射：

- `add` / `add_factory`：注册策略类名称，以及实际工厂函数。
- `has` / `get_all_strategy_class_names`：查询当前可用策略类型。
- `create`：根据类名、`OptionStrategyEngine*`、策略名、组合名与参数字典，构建策略实例。

`REGISTER_STRATEGY(ClassName)` 宏用于简化注册逻辑：

- 在 `strategy_registry.cpp` 中集中过一行写入：
  - `REGISTER_STRATEGY(HighFrequencyMomentumStrategy);`
- 工厂函数会将 `void* engine` 强转为 `core::OptionStrategyEngine*`，并构造策略。

这一机制使得：

- Backtest 与 Live 可以通过同一个类名创建策略实例。
- 新策略的接入只需添加头文件 include + 注册宏，改动局部且易于维护。

##### 2.3.2 策略模板与高频动量策略

`HighFrequencyMomentumStrategy` 派生自 `OptionStrategyTemplate`，其头文件中可以看到若干关键参数：

- **仓位控制参数**：
  - `position_size_`、`max_holding_minutes_`、`cooldown_minutes_`。
- **信号阈值**：
  - `momentum_threshold_`、`iv_change_threshold_`。
- **风控参数**：
  - `profit_target_pct_`、`stop_loss_pct_`。

策略内部维护：

- `chain_symbols_`：订阅的期权链列表。
- `entry_price_`、`entry_time_`、`last_exit_time_` 等交易状态。
- `last_underlying_price_`、`last_iv_call_`、`last_iv_put_` 等用于判断动量与 IV 变化的状态量。

运行时，OptionStrategyEngine 会周期性调用 `on_timer_logic`，策略内部通过：

- `check_entry_signals()`：基于近期价格与 IV 变化决定是否开仓。
- `check_exit_conditions()`：基于止盈/止损/时间到期等条件决定是否平仓。
- `enter_position` / `enter_straddle` / `reset_position`：通过 RuntimeAPI 下单或关闭头寸。

策略本身只感知：

- 从组合视图中读取到的价格与 Greeks。
- 从 `StrategyHolding` 中读取到的当前持仓。
- 自身的参数与内部状态。

所有订单执行细节（撮合价、手续费、滑点）以及数据来源（实盘 vs 回测）均由运行时负责。

---

#### 2.4 Backtest 层：回测引擎与数据加载

Backtest 层围绕 `BacktestEngine` 构建一条完整的「数据 → 策略 → 订单执行 → 绩效统计 → JSON 输出」流水线。

##### 2.4.1 回测入口（`entry_backtest.cpp`）

`entry_backtest` 可执行程序主要职责：

- 解析命令行参数：
  - 支持单文件模式与 `--files` 多文件模式。
  - 解析 `strategy_name`、`fill_mode`（`mid|bid|ask`）、`fee_rate`、`risk_free_rate`、`iv_price_mode`、`--log`、以及一组 `key=value` 的策略参数。
- 构建一个或多个 `BacktestEngine` 实例（支持并行处理多文件）。
- 为每个 engine 注册 timestep 回调，采集逐帧指标（PnL、Delta、Theta、Gamma、Fees）。
- 汇总每日结果与整体指标（总 PnL、净 PnL、最大回撤、Sharpe 等），并以 JSON 形式输出到 stdout。

它将所有 UI 与交互层面的复杂性屏蔽在外，对上层 Python/后端暴露一个统一的命令行协议与 JSON 输出格式。

##### 2.4.2 BacktestEngine 结构（`runtime/backtest/engine_backtest.hpp/cpp`）

`BacktestEngine` 是回测模式下的顶层控制器，其核心成员包括：

- `std::unique_ptr<MainEngine> main_engine_`：回测专用 MainEngine。
- `std::unique_ptr<EventEngine> event_engine_`：内部事件驱动（回测版）。
- 策略名称与参数：`strategy_name_`、`strategy_setting_`。
- 执行与绩效统计字段：`current_timestep_`、`current_pnl_`、`max_delta_`、`max_gamma_`、`max_theta_`、`max_drawdown_` 等。
- 费用与成交统计：`fee_rate_`、`cumulative_fees_`、`total_orders_`（订单数量）。

**关键方法**：

- `load_backtest_data(parquet_path, underlying_symbol)`：
  - 构造或复用 `BacktestDataEngine`（定义于 `infra/marketdata/engine_data_historical.hpp`），从 parquet 读取带有 `ts_recv` 时间戳的行情与希腊值。
  - 将数据映射到 `PortfolioSnapshot` 或类似结构，为后续逐帧回放做准备。

- `add_strategy(strategy_name, setting)`：
  - 与 core 层 `OptionStrategyEngine` 对接，创建策略实例并注入参数。

- `configure_execution(fill_mode, fee_rate)`：
  - 指定撮合价策略（`mid`/`bid`/`ask`）与手续费率。

- `run()`：
  - 驱动数据回放循环，对每一时间步：
    - 更新组合快照。
    - 驱动策略 `on_timer` 与订单执行。
    - 根据订单请求调用 `execute_order`，计算成交价与费用。
    - 统计逐步 PnL、Greeks、最大回撤等。
    - 触发已注册的 timestep 回调，将相关指标收集给调用方。

- `reset()`：
  - 在多文件并行回测场景下，允许复用 engine 实例，但清理内部状态与数据。

##### 2.4.3 Backtest MainEngine（`runtime/backtest/engine_main.cpp`）

回测模式下的 `MainEngine` 与 live 版有明显不同：

- 不连接真实网关，而是通过内部逻辑完成撮合。
- 依赖一个 `BacktestDataEngine`（实现在 `infra/marketdata/engine_data_historical.cpp`）来提供时间序列数据。
- 持有：
  - `engines::PositionEngine`：策略持仓管理。
  - `engines::LogEngine`：日志记录（默认禁用，可通过 `set_log_level` 打开）。
  - `core::OptionStrategyEngine`：统一策略引擎。

**特征**：

- 使用 `backtest::EventEngine`；回测调度主要由 `BacktestEngine::run()` 显式驱动（无独立事件线程）。
- `load_backtest_data()` 将 parquet 中的数据转成内部 `BacktestDataEngine` 的帧表示。
- `send_order()` 不直接发送到外部，而由 `BacktestEngine` 提供的回调 `execute_order` 处理：
  - 根据 `fill_mode` 与当前 `PortfolioData` 中的 bid/ask/mid 决定成交价。
  - 根据 `fee_rate` 计算手续费并累计。
  - 更新 `OptionStrategyEngine` 中的订单与成交状态。

这种设计保证回测环境可以完全在内存中重放，并在不依赖任何外部系统的前提下，对策略进行全流程验证。

---

#### 2.5 Live 层：实盘引擎与 gRPC 接口

Live 层将核心策略引擎接入 IB TWS 与 PostgreSQL 数据库，并通过 gRPC 向后端暴露控制面与查询面。

##### 2.5.1 实盘入口（`entry_live.cpp` 与 `entry_live_grpc.cpp`）

**`entry_live.cpp`** 负责：

- 安装 SIGINT/SIGTERM 处理，将 `g_running` 标志置 false 以触发优雅退出。
- 加载 `.env` 文件（当前目录及上级目录），为 `DatabaseEngine` 提供如 `DATABASE_URL` 等连接信息。
- 构造 `engines::EventEngine` 与 `engines::MainEngine`。
- 调用 `main_engine.connect()` 建立与 IB/TWS 的连接。
- 进入简单的循环，保持进程存活，直到收到终止信号，然后：
  - 调用 `main_engine.disconnect()` 与 `main_engine.close()` 完成资源释放。

**`entry_live_grpc.cpp`** 在上述基础上增加 gRPC：

- 创建 `EventEngine` 与 `MainEngine`。
- 将 `MainEngine*` 注入 `EventEngine` 与 `GrpcLiveEngineService`。
- 使用 `grpc::ServerBuilder`：
  - 监听 `0.0.0.0:50051` 端口。
  - 注册 `GrpcLiveEngineService`。
  - 调用 `server->Wait()` 阻塞至进程被终止。
- 在 gRPC 服务器退出前调用 `disconnect()` 与 `close()`，保证资源清理。

##### 2.5.2 Live MainEngine（`runtime/live/engine_main.cpp`）

live 版 `MainEngine` 是实盘模式下所有引擎的总控：

- 构造过程中：
  - 若未显式传入 `EventEngine*`，则内部创建一个 `EventEngine` 并启动其线程。
  - 创建：
    - `LogEngine`：负责日志输出与过滤。
    - `PositionEngine`：维护策略持仓。
    - `DatabaseEngine`：从 PostgreSQL 加载合约、保存订单与成交历史。
    - `MarketDataEngine`：基于 Contract/Snapshot 事件构建并更新 `PortfolioData`。
    - `IbGateway`：与 IB TWS 通信的网关。
    - `core::OptionStrategyEngine`：统一策略引擎，并将 RuntimeAPI 中各能力绑定到上述组件。
  - 调用 `db_engine_->load_contracts()`：
    - 从数据库读取所有合约信息。
    - 通过 `EventEngine` 派发 `Contract` 事件，最终在 `MarketDataEngine::process_contract` 中构建组合与期权链结构。

**公开能力**：

- **网关控制**：
  - `connect` / `disconnect`。
  - `query_account` / `query_position`。

- **订单控制**：
  - `send_order` / `cancel_order`，内部调用 `IbGateway`。
  - `get_order` / `get_trade`：从 `OptionStrategyEngine` 中查询。

- **行情与组合**：
  - `start_market_data_update` / `stop_market_data_update`（结合外部数据源，可定时推送 Snapshot）。
  - `get_portfolio` / `get_all_portfolio_names`。
  - `get_contract` / `get_all_contracts`。

- **策略更新事件流**：
  - 接收 `StrategyUpdateData` 并写入内部队列：
    - `on_strategy_event` 将更新事件推入 deque，并通过条件变量唤醒消费端。
    - `pop_strategy_update` 提供阻塞/带超时的获取方法，供 gRPC `StreamStrategyUpdates` 使用。

MainEngine 还提供日志写入封装（`write_log`/`put_log_intent`）以及对 HedgeEngine/ComboBuilderEngine 的懒初始化访问。

##### 2.5.3 实盘 EventEngine（`runtime/live/engine_event.hpp/cpp`）

Live `EventEngine` 实现了完整的异步事件处理：

- 内部维护：
  - 一个事件队列 `queue_` 与互斥锁 + 条件变量。
  - 一个 dispatch 线程处理业务事件。
  - 一个定时器线程定期触发 `Timer` 事件。

- **接收入口**：
  - `put_event` / `put`：接收来自 MainEngine 或其他组件的事件，并推入队列。
  - `put_intent_send_order` / `put_intent_cancel_order` / `put_intent_log`：
    - 用于接收来自上游（例如 IB 回调、外部 controller）的「意图」，并转换为事件或直接调用 `MainEngine`。

- **调度逻辑**（按事件类型固定顺序分发）：
  - `dispatch_timer`：调用 `OptionStrategyEngine::on_timer()`，驱动所有策略执行定时逻辑（如风控、条件单等）。
  - `dispatch_order`：调用 `OptionStrategyEngine::process_order()`，并在必要时通过 `DatabaseEngine` 持久化。
  - `dispatch_trade`：调用 `OptionStrategyEngine::process_trade()`，并写入 DB。
  - `dispatch_contract`：调用 `MarketDataEngine::process_contract()`，构建或更新组合结构。
  - `dispatch_snapshot`：调用 `PortfolioData::apply_frame()`，更新价格与 Greeks。

通过 EventEngine，live 模式下所有外部驱动因素（行情、订单状态、定时任务）都转化为统一的事件流。

##### 2.5.4 MarketDataEngine：组合与行情更新（`infra/marketdata/engine_data_tradier.hpp`）

实盘使用的 `MarketDataEngine` 定义于 `infra/marketdata/engine_data_tradier.hpp`（Tradier 命名仅为文件区分；接口为通用组合与行情维护）：

- `process_contract`：
  - 处理 `EventType::Contract` 事件。
  - 在内部 `contracts_` 集合中登记合约。
  - 基于合约的 `option_portfolio` / `option_underlying` 等信息，构造或更新对应 `PortfolioData`：
    - 为每个组合内的期权添加 `OptionData`。
    - 配置组合标的 `UnderlyingData`。
    - 在结构构建完毕后调用 `finalize_chains`，确保 `option_apply_order_` 稳定。

- `subscribe_chains` / `unsubscribe_chains`：
  - 记录策略与其感兴趣的期权链 symbols，用于上游行情源按需推送数据。

- `start_market_data_update` / `stop_market_data_update`：
  - 与外部行情源（未在此工程中实现）配合，控制 Snapshot 推送节奏。

该引擎不直接处理 Tick/Greeks，而是依赖已经整理好的 `PortfolioSnapshot` 来更新内部结构，保持接口简单、稳定。

##### 2.5.5 DatabaseEngine：PostgreSQL 持久化（`infra/db/engine_db_pg.hpp/cpp`）

`DatabaseEngine` 负责与 PostgreSQL 的所有交互：

- 管理合约表，支撑：
  - `load_contracts`：启动时读取合约，并通过 `MainEngine::put_event(EventType::Contract, ContractData)` 按固定顺序发出事件，驱动 `MarketDataEngine` 构建组合。
  - `save_contract_data` / `load_contract_data`：读写合约元数据。

- 管理订单与成交表，支撑：
  - `save_order_data(strategy_name, OrderData)`。
  - `save_trade_data(strategy_name, TradeData)`。
  - `get_all_history_orders` / `get_all_history_trades`。
  - `wipe_trading_data` 用于清理历史记录。

- 内部由 `pqxx::connection` 管理数据库连接，并持有互斥锁 `db_mutex_` 确保线程安全。

数据库连接字符串可由构造函数参数或环境变量（如 `.env` 中的 `DATABASE_URL`）提供。

##### 2.5.6 IbGateway 与 IB API 抽象（`infra/gateway/engine_gateway_ib.hpp/cpp`）

`IbGateway` 是对 IB/TWS 连接的包装：

- 内部通过 `IbApi` 抽象类与具体实现 `IbApiTws` 解耦。
- 提供：
  - `connect` / `disconnect`。
  - `send_order` / `cancel_order`。
  - `query_account` / `query_position` / `query_portfolio`。
  - `process_timer_event`：周期性调用，确保 TWS 消息队列被持续消费。

`IbGateway` 与 `MainEngine` 的关系：

- 持有一个 `MainEngine* main_engine_`，在接收到订单、成交或合约更新时，通过 `on_order` / `on_trade` / `on_contract` 向主引擎发出事件。
- 默认设置（host、port、client_id、account）由 `Setting` 结构体管理，可由上层配置。

##### 2.5.7 GrpcLiveEngineService 与 proto 接口（`runtime/live/engine_grpc.hpp` + `proto/otrader_engine.proto`）

`GrpcLiveEngineService` 实现 proto 中定义的 `EngineService`：

- 调用 `MainEngine` 提供的接口实现：
  - `GetStatus`：返回是否运行中、是否连接 IB，以及简单文本描述。
  - `ListStrategies`：枚举当前加载的策略（名/class/portfolio/status）。
  - `ConnectGateway` / `DisconnectGateway`：控制 `IbGateway`。
  - `StartMarketData` / `StopMarketData`：调用 `MarketDataEngine` 相关控制。
  - `GetOrdersAndTrades`：拉取当前订单与成交列表。
  - `ListPortfolios` / `GetPortfoliosMeta`：列出所有组合名称。
  - `ListStrategyClasses`：查询所有可用策略类名（来自 `StrategyRegistry`）。
  - `GetRemovedStrategies`：查看已移除策略列表。
  - `AddStrategy` / `RestoreStrategy` / `InitStrategy` / `RemoveStrategy` / `DeleteStrategy`：管理策略生命周期。
  - `GetStrategyHoldings`：返回当前所有策略持仓的 JSON 描述。

- 流式接口：
  - `StreamLogs`：将日志行作为 `LogLine` 流式返回。
  - `StreamStrategyUpdates`：从 `MainEngine` 的 `StrategyUpdateData` 队列中消费数据，向外输出策略更新事件。

proto 文件 `otrader_engine.proto` 则定义了所有消息结构与 RPC 签名，确保 C++ 与后端服务之间有稳定的契约。

---

### 3. 运行时视图

本节从「回测模式」与「实盘模式」两个角度，描述运行时组件的协作关系与典型调用路径。

#### 3.1 回测模式运行时

**核心组件**：

- `entry_backtest` 可执行程序。
- `backtest::BacktestEngine`。
- 回测版 `backtest::MainEngine` 与 `backtest::EventEngine`。
- `core::OptionStrategyEngine` 与注册的策略实例。
- `BacktestDataEngine`（`infra/marketdata/engine_data_historical`）与 `PortfolioData`。

##### 3.1.1 初始化阶段

1. `entry_backtest` 解析命令行参数与环境变量（如 `BACKTEST_LOG`）。
2. 为并行执行场景预创建若干 `BacktestEngine` 实例，每个实例内部：
   - 构造 `MainEngine`，并注入 `OptionStrategyEngine` 与 `PositionEngine`、`LogEngine` 等。
   - 通过 RuntimeAPI 将 `send_order`、`write_log`、`get_portfolio`、`get_contract` 等能力暴露给 `OptionStrategyEngine`。

##### 3.1.2 数据加载与组合构建

对于每个回测文件：

1. 调用 `BacktestEngine::load_backtest_data(parquet_path)`：
   - 在 `MainEngine` 内部创建/复用 `BacktestDataEngine`。
   - 解析 parquet 中的行情、希腊值与元数据。
   - 将其组织为时间有序的帧（通常对应分钟级别）。
2. `BacktestDataEngine` 利用 `PortfolioData` 与 `ChainData` 结构构建组合视图：
   - 初始时基于合约信息构造组合。
   - 后续每帧更新使用类似 `PortfolioSnapshot` 的方式写入价格与 Greeks。

##### 3.1.3 策略加载与执行

1. `BacktestEngine::add_strategy(strategy_name, setting)`：
   - 调用 `OptionStrategyEngine::add_strategy(class_name, portfolio_name, setting)`。
   - 通过 `StrategyRegistry::create` 根据类名构造具体策略实例。
2. 在运行 loop 中，`BacktestEngine::run()` 会对每一时间步执行：
   - 从 `BacktestDataEngine` 读取当前帧的组合快照并应用到 `PortfolioData`。
   - 调用 `OptionStrategyEngine::on_timer()`：
     - 逐个策略调用其 `on_timer_logic`，执行交易逻辑与风控。
   - 对策略调用产生的 `OrderRequest`，通过 `BacktestEngine::execute_order`：
     - 根据 fill_mode 从 bid/ask/mid 选择成交价。
     - 计算手续费并更新 `cumulative_fees_`。
     - 构造 `OrderData` 与 `TradeData`，调用 `OptionStrategyEngine::process_order/process_trade`。
   - 在每步末尾，更新汇总指标，并调用所有注册的 timestep 回调。

##### 3.1.4 结果汇总与输出

在所有文件处理完毕后：

- `entry_backtest` 聚合每个文件的 `BacktestResult`：
  - 按输入文件顺序排序为「日序列」。
  - 计算总 PnL、净 PnL（扣手续费）、最大回撤、Sharpe 等。
  - 构造逐日汇总（file、pnl、net_pnl、fees、orders、timesteps）。
  - 对所有时间步 metrics 按 timestamp 排序，形成统一的时间序列。
- 将上述结果封装为结构化 JSON 输出到 stdout，并在 stderr 输出进度更新。

回测运行时与实盘相比，没有外部依赖，所有动作都在内存中可控地重放，便于调试与自动化测试。

---

#### 3.2 实盘模式运行时（非 gRPC）

实盘运行时由 `entry_live` 启动，内部组件包括：

- `engines::EventEngine`（带事件队列与定时器）。
- `engines::MainEngine`（live 版本）。
- `engines::DatabaseEngine`。
- `engines::MarketDataEngine`。
- `engines::IbGateway` 与 `IbApiTws`。
- `core::OptionStrategyEngine` 与策略实例。

##### 3.2.1 初始化与合约加载

1. `entry_live` 安装信号处理器、加载 `.env`，然后创建：
   - `EventEngine event_engine(1)`。
   - `MainEngine main_engine(&event_engine)`。
2. `MainEngine` 构造函数中：
   - 将 `event_engine` 作为 `IEventEngine` 启动其工作线程与定时器线程。
   - 创建 `LogEngine`、`PositionEngine`、`DatabaseEngine`、`MarketDataEngine`、`IbGateway`、`OptionStrategyEngine`。
   - 配置 RuntimeAPI，将所有能力注入 `OptionStrategyEngine`。
   - 调用 `db_engine_->load_contracts()`：
     - 从 PostgreSQL 读取合约列表。
     - 对每个合约构造 `Event(EventType::Contract, ContractData)` 并通过 `EventEngine` 发送。
     - `EventEngine::dispatch_contract` 调用 `MarketDataEngine::process_contract`：
       - 构建 `PortfolioData`、`ChainData` 和 `OptionData` 结构。

此时，组合结构在内存中已经成型，但价格与 Greeks 仍处于初始状态。

##### 3.2.2 与 IB/TWS 的连接

1. `entry_live` 调用 `main_engine.connect()`：
   - `IbGateway::connect()` 使用内部 `IbApi` 与 IB/TWS 建立 TCP 会话。
   - 成功后，IB API 会开始推送订单、成交、合约信息，以及行情数据。
2. IB 回调通过 `IbApiTws` 调用 `IbGateway` 的私有回调：
   - `on_order(OrderData)`、`on_trade(TradeData)`、`on_contract(ContractData)` 等。
3. `IbGateway` 在这些回调中调用：
   - `main_engine->put_event(EventType::Order, order)`。
   - `main_engine->put_event(EventType::Trade, trade)`。
   - `main_engine->put_event(EventType::Contract, contract)`（对于新增或更新合约）。

##### 3.2.3 实盘事件流

`EventEngine` 在其主线程中循环：

1. 从队列中取出事件。
2. 根据事件类型调用相应 `dispatch_*` 方法：
   - **订单事件**：
     - 调用 `OptionStrategyEngine::process_order` 更新内部 OMS 状态。
     - 调用 `DatabaseEngine::save_order_data` 将订单写入 PostgreSQL。
   - **成交事件**：
     - 调用 `OptionStrategyEngine::process_trade` 更新持仓与指标。
     - 调用 `DatabaseEngine::save_trade_data` 将成交写入数据库。
   - **合约事件**：
     - 调用 `MarketDataEngine::process_contract` 更新组合结构。
   - **快照事件**：
     - 调用 `PortfolioData::apply_frame(snapshot)` 更新行情与 Greeks。

定时器线程则周期性触发 `Timer` 事件：

- 在 `dispatch_timer` 中调用 `OptionStrategyEngine::on_timer()`：
  - 各策略执行自身的定时逻辑（如轮询检查风险指标、超时平仓、逐步调整头寸）。

##### 3.2.4 策略与订单执行

在实盘运行时，策略与订单执行路径为：

1. 策略在 `on_init_logic` / `on_timer_logic` 等回调中：
   - 读取 `PortfolioData` 与 `StrategyHolding`。
   - 调用 `OptionStrategyEngine::send_order` 或 `send_combo_order` 提交订单。
2. `OptionStrategyEngine` 将订单请求提交给 runtime（`RuntimeAPI::send_order`）：
   - 对 live 版 MainEngine，最终调用 `IbGateway::send_order`。
3. `IbGateway` 通过 `IbApi` 发送给 TWS，等待成交与订单状态回报。
4. TWS 的回报再次通过事件流（`Order`/`Trade`）回到 `OptionStrategyEngine`，策略通过持仓或订单查询接口感知实际成交结果。

##### 3.2.5 停止与资源回收

当 `entry_live` 收到终止信号时：

1. 将 `g_running` 置为 false，退出主循环。
2. 调用 `main_engine.disconnect()`：
   - 关闭与 IB/TWS 的连接。
3. 调用 `main_engine.close()`：
   - 关闭 `OptionStrategyEngine`，停止对策略的调用。
   - 关闭 `DatabaseEngine`（关闭数据库连接）。
   - 让 `EventEngine` 停止事件与定时器线程。

---

#### 3.3 实盘 gRPC 模式运行时

在 gRPC 模式下，`entry_live_grpc` 将 `MainEngine` 嵌入到一个 gRPC 服务器中：

- 后端服务通过 `EngineService` 接口控制 live 引擎：
  - 连接/断开 gateway。
  - 启停策略与行情。
  - 查询当前订单、成交与持仓。
  - 拉取日志与策略更新流，用于前端展示。

**典型调用路径（示例）**：

- 后端调用 `ConnectGateway`：
  - `GrpcLiveEngineService::ConnectGateway` → `main_engine->connect()`。

- 后端调用 `AddStrategy`：
  - proto 请求体携带 `strategy_class`、`portfolio_name` 与 `setting_json`。
  - Service 将 `setting_json` 解析为参数字典，调用 `main_engine` 对接 `OptionStrategyEngine::add_strategy`。

- 后端调用 `StreamStrategyUpdates`：
  - Service 在一个 loop 中反复调用 `main_engine->pop_strategy_update`，将 `StrategyUpdateData` 转换为 proto `StrategyUpdate` 并写入 gRPC 流。

gRPC 模式不改变内核逻辑，只是在外层加了一层远程控制与状态暴露的包装。

---

### 4. 数据与事件流

本节从「数据结构演化」与「事件生命周期」两方面，总结 Otrader 的数据与事件流设计。

#### 4.1 合约与组合结构演化

1. **合约初始化阶段**：
   - Backtest 模式中，合约可以由回测数据加载逻辑直接构建。
   - Live 模式中，合约由 `DatabaseEngine::load_contracts` 从 PostgreSQL 读取，并转化为 `ContractData` 事件。
2. **组合结构构建**：
   - `MarketDataEngine::process_contract` 或对应 backtest 逻辑：
     - 对每个合约决定其所属组合（`option_portfolio`）、标的（`option_underlying`）与链（`option_index`）。
     - 构建或更新 `PortfolioData`、`UnderlyingData` 与 `ChainData`。
   - 在初始构建完成后调用 `PortfolioData::finalize_chains`，锁定 `option_apply_order_`。
3. **运行期结构维护**：
   - 新合约的出现会触发组合结构的扩展。
   - 到期合约可由 `DatabaseEngine::cleanup_expired_options` 或其他定期任务清理。

这种模型使合约信息的演化具有：「数据库 → ContractData 事件 → PortfolioData 结构 → Snapshot 应用」的清晰流向。

#### 4.2 行情与快照流

行情与 Greeks 的流动路径可以归纳为：

1. **数据源产生帧数据**：
   - 回测：由 `BacktestDataEngine` 读取 parquet 帧。
   - 实盘：由外部行情处理进程/组件，根据 Tick/Greeks 生成 `PortfolioSnapshot`。
2. **快照事件分发**：
   - 回测：在 `BacktestEngine::run()` 中，直接调用 `PortfolioData::apply_frame` 或通过 `EventEngine` 分发。
   - 实盘：外部组件构造 `Event(EventType::Snapshot, snapshot)` 并通过 `EventEngine::put_event` 发送。
3. **组合更新与策略感知**：
   - `PortfolioData::apply_frame` 按固定顺序将 snapshot 写入所有期权与标的。
   - 策略在下一次 `on_timer` 调用或自定义触发点，从组合中读取最新价格与 Greeks，用于生成交易信号。

这种「快照帧」设计减少了 per-option 粒度的事件数量，适合高频期权组合作用场景。

#### 4.3 订单与成交生命周期

订单从生成到归档的典型生命周期：

1. **策略生成订单请求**：
   - 策略调用 `OptionStrategyEngine::send_order`。
   - OptionStrategyEngine 通过 RuntimeAPI 调用 `MainEngine` 的实际下单实现。
2. **运行时执行下单**：
   - 回测：
     - `BacktestEngine::execute_order` 直接在内存中撮合出成交价与成交量。
     - 立即构造 `OrderData` + `TradeData`，反向调用 `OptionStrategyEngine::process_order/process_trade`。
   - 实盘：
     - `IbGateway::send_order` 将订单发送到 TWS。
     - 等待 TWS 汇报订单状态与成交。
3. **订单与成交事件回流**：
   - 所有订单状态变化与成交事件重新封装为 `Event(EventType::Order or Trade, payload)`，交给 EventEngine。
   - EventEngine 调用 `OptionStrategyEngine::process_order/process_trade` 更新内部 OMS。
4. **持久化与对外暴露**：
   - Live 模式中，`DatabaseEngine::save_order_data/save_trade_data` 将数据写入 PostgreSQL。
   - gRPC `GetOrdersAndTrades` 调用从 DB 或 OptionStrategyEngine 集合中读取并转化为 proto `OrderRecord`、`TradeRecord`。

这样，无论在回测还是实盘环境中，策略侧看到的订单生命周期语义是一致的。

#### 4.4 策略更新与监控流

策略在运行过程中，可以通过两种方式对外暴露状态变化：

- **策略持仓**：
  - `StrategyHolding` 结构体记录了标的与所有期权/组合的持仓与 Greeks。
  - Live 模式下，gRPC `GetStrategyHoldings` 将这些信息序列化为 JSON 以便前端展示。

- **策略更新事件**：
  - 策略通过 `RuntimeAPI::put_strategy_event` 提交 `StrategyUpdateData`。
  - MainEngine 将其推入一个 `deque`，并提供 `pop_strategy_update` 供消费。
  - gRPC `StreamStrategyUpdates` 使用一个 long-lived stream 将这些事件推送到外部。

这一机制方便构建「策略看板」、「风控监控」等上层产品。

---

### 5. 扩展性与演进方向

#### 5.1 策略扩展

新增策略的标准流程：

1. 在 `strategy/` 下创建新的策略类头文件与实现文件，派生自 `OptionStrategyTemplate`。
2. 在类中通过构造函数接收：
   - `core::OptionStrategyEngine*`。
   - `strategy_name`。
   - `portfolio_name`。
   - `setting`（参数字典）。
3. 实现必要的生命周期与信号逻辑（`on_init_logic`、`on_timer_logic`、`on_stop_logic` 等）。
4. 在 `strategy/strategy_registry.cpp` 中：
   - `#include "your_strategy.hpp"`。
   - 添加一行 `REGISTER_STRATEGY(YourStrategyClassName);`。

完成上述步骤后：

- 回测模式可以在 `entry_backtest` 中通过策略类名进行回测。
- Live 模式可以通过 gRPC 接口 `ListStrategyClasses` 发现新策略类型，并通过 `AddStrategy` 创建与配置。

#### 5.2 数据源扩展

当前设计已经为多种数据源预留了扩展空间：

- 回测：
  - `BacktestDataEngine` 可扩展支持多标的、跨品种与更细粒度的频率。
  - 可增加滑点与成交优先级模型。
- 实盘：
  - `MarketDataEngine` 以 `PortfolioSnapshot` 为输入，数据源可以是：
    - IB 实时数据经过预处理的快照。
    - 外部定价模型或聚合服务输出的 Greeks 帧。
  - 只要能组装出 `PortfolioSnapshot` 并发出 `EventType::Snapshot` 事件，策略与核心逻辑均无需变更。

#### 5.3 风控与监控扩展

基于现有结构，可以逐步引入：

- 账户级风控引擎：
  - 在 `MainEngine` 或单独的 `RiskEngine` 中，读取所有策略持仓与 PnL，设置全局风控阈值（如单日最大亏损、最大仓位等）。
  - 为 `OptionStrategyEngine` 增加风控反馈接口，用于拒绝或缩减部分下单请求。

- 多级监控：
  - 在 gRPC 层扩展更多 RPC，例如实时推送全局 PnL 曲线、风险暴露等。
  - 引入订阅机制，使前端可以按策略/组合选择订阅粒度。

#### 5.4 多资产与多账户支持

尽管当前设计以单账户、单底层为主，但在结构上已经具备一定的可扩展性：

- `PortfolioData` 可以按不同组合名区分不同底层与资产集合。
- `StrategyHolding` 可以扩展字段以支持多账户区分。
- `IbGateway` 可以扩展为多实例或多账号配置。

这些演进仅需在现有数据结构与接口上做增量设计，无需推翻当前主干架构。

---

### 6. 小结

本架构文档从模块视图、运行时视图、数据与事件流以及扩展性四个方面，对 Otrader C++ 子系统进行了整体梳理。  
核心特点可以概括为：

- **统一策略引擎**：backtest 与 live 共用 `OptionStrategyEngine` 与策略模板，只在 RuntimeAPI 实现上有所差异。
- **强类型组合模型**：以 `PortfolioData` 为中心，将合约、期权链、标的与快照统一纳入一个可组合的视图中。
- **事件驱动运行时**：使用 `EventEngine` 协调订单、成交、合约与快照流，清晰地分离生产者与消费者。
- **良好的扩展边界**：策略、数据源、风控与监控都可以在现有结构基础上平滑演进。

在后续的演进中，可以围绕：

- 更丰富的风险度量与报表能力。
- 对多品种、多账户、多市场的支持。
- 更灵活的策略参数管理与运行配置。

进一步增强 Otrader 在生产环境中的适用性与可维护性。

