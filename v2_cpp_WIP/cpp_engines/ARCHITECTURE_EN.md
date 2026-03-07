# Otrader Architecture Document

---

## 0. File Tree

```
Otrader/
├── entry_backtest.cpp                  					#   Backtest executable entry
├── entry_live.cpp                      					#   Live executable entry (no gRPC)
├── entry_live_grpc.cpp                 					#   Live + gRPC service entry
│
├── runtime/                            					#   Runtime: backtest vs live differences
│   ├── backtest/
│   │   ├── engine_backtest.{cpp,hpp}   					#   Backtest top-level controller
│   │   ├── engine_event.{cpp,hpp}      					#   Backtest event engine (sync dispatch)
│   │   └── engine_main.{cpp,hpp}       					#   Backtest MainEngine
│   │
│   └── live/
│       ├── engine_event.{cpp,hpp}      					#   Live event engine (queue + worker thread)
│       ├── engine_main.{cpp,hpp}       					#   Live MainEngine
│       └── engine_grpc.{cpp,hpp}       					#   gRPC service implementation
│
├── infra/                              					#   Infrastructure: data, persistence, gateway
│   ├── marketdata/
│   │   ├── engine_data_historical.{cpp,hpp}  				#   Backtest data engine (parquet → snapshot)
│   │   └── engine_data_tradier.{cpp,hpp}     				#   Live market/portfolio engine
│   ├── db/
│   │   └── engine_db_pg.{cpp,hpp}       					#   PostgreSQL contract/order/trade
│   └── gateway/
│       └── engine_gateway_ib.{cpp,hpp}   					#   IB TWS gateway
│
├── core/                               					#   Domain core: strategy engines and helpers
│   ├── engine_execution.{cpp,hpp}      					#   Order/trade cache and order submission
│   ├── engine_option_strategy.{cpp,hpp} 					#   Unified strategy engine + RuntimeAPI
│   ├── engine_position.{cpp,hpp}       					#   Strategy position management
│   ├── engine_hedge.{cpp,hpp}          					#   Hedging engine
│   ├── engine_combo_builder.{cpp,hpp}  					#   Combo leg builder
│   └── engine_log.{cpp,hpp}            					#   Log engine
│
├── strategy/                           					#   Strategy implementations and registry
│   ├── factory/                        					#   Concrete strategy implementations
│   │   ├── straddletest.{cpp,hpp}
│   │   ├── iv_mean_revert.{cpp,hpp}
│   │   └── ...
│   ├── template.{cpp,hpp}              					#   Strategy template base class
│   └── strategy_registry.{cpp,hpp}     					#   Strategy class name → factory
│
├── proto/                              					#   gRPC service definitions (.proto)
├── tests/                              					#   Backtest and live tests
├── thirdparty/                         					#   Third-party deps (e.g. IB JTS)
└── utilities/                          					#   Shared data models and abstractions
    ├── event.hpp                       					#   Event, EventType, EventPayload
    ├── object.hpp                      					#   OrderData, TradeData, ContractData, PortfolioSnapshot
    ├── portfolio.hpp                   					#   PortfolioData
    ├── base_engine.hpp                 					#   MainEngine virtual interface, BaseEngine base class
    └── constant.hpp etc                					#   Enums and constants
```


---

## 1. System Overview and Core Concepts

Otrader is a C++20 trading engine for **options portfolio trading**. Backtest and live share the same domain core (Core + Strategy); differences lie only in the runtime: data source, clock driver, order execution, and persistence.

### 1.1 Functional Scope and Boundaries

| Mode | Function | Entry | Boundary |
|------|----------|-------|----------|
| **Backtest** | Single-process historical replay, strategy execution, order matching, performance stats | `entry_backtest` | CLI launch, JSON to stdout; no network or database |
| **Live (no gRPC)** | Market data from MarketDataEngine (e.g. Tradier); submit orders and record fills via IB TWS | `entry_live` | .env and IbGateway; in-process event loop only |
| **Live (gRPC)** | Same as above + external control | `entry_live_grpc` | EngineService at `0.0.0.0:50051`; contracts and orders/trades in PostgreSQL |

### 1.2 Architecture Layers

| Layer | Responsibility | Main Components |
|-------|----------------|-----------------|
| **Domain Core** | Pure logic: receive Events, update state, emit Intents; no direct order submission, DB access, or gateway access | OptionStrategyEngine, PositionEngine, HedgeEngine, ComboBuilderEngine, LogEngine, ExecutionEngine, Strategy implementations |
| **Runtime** | Source data and clock, execute Intents from Core, turn execution results into Events for feedback | BacktestEngine, EventEngine (backtest/live), MainEngine (backtest/live), gRPC Service |
| **Infrastructure** | Data sources, persistence, gateway | BacktestDataEngine, MarketDataEngine, DatabaseEngine, IbGateway |
| **Utilities** | Shared data models, event and engine abstractions | event.hpp, portfolio.hpp, object.hpp, base_engine.hpp, constant.hpp, etc. |

### 1.3 Engine References and Interactions

**Ownership**: MainEngine owns all engine instances (EventEngine, LogEngine, OptionStrategyEngine, PositionEngine, HedgeEngine, ComboBuilderEngine, ExecutionEngine, plus Infrastructure: DatabaseEngine, MarketDataEngine, IbGateway). EventEngine holds a non-owning pointer to MainEngine and accesses engines via MainEngine accessors; it does not hold engine instances directly.

**Event flow**: External producers (BacktestEngine, MarketDataEngine, IbGateway) call `put_event` on MainEngine, which forwards to EventEngine. EventEngine dispatches by type: Snapshot → portfolio `apply_frame`; Timer → OptionStrategyEngine (which drives strategies, PositionEngine, HedgeEngine, and ExecutionEngine); Order/Trade → ExecutionEngine and PositionEngine for state update, then OptionStrategyEngine for strategy `on_order`/`on_trade` callbacks.

**Intent flow**: Strategies and HedgeEngine produce Intents via RuntimeAPI (send_order, cancel_order, write_log). RuntimeAPI is wired to MainEngine: order/cancel intents go to EventEngine's `put_intent` (live) or BacktestEngine's matching path (backtest); log intents go to LogEngine. OptionStrategyEngine receives RuntimeAPI at construction; HedgeEngine and ComboBuilderEngine are obtained via SystemAPI when needed.

**Core isolation**: OptionStrategyEngine, PositionEngine, HedgeEngine, ComboBuilderEngine, LogEngine, and ExecutionEngine do not hold MainEngine or EventEngine. They receive capabilities via RuntimeAPI or caller-passed callbacks (e.g. `get_portfolio`, `send_impl`).

### 1.4 Event-In, Intent-Out

**Principle**: Inputs are Events; outputs are Intents. Fixed dispatch order: Snapshot → update portfolio; Timer → strategy, position, hedge, execute intents; Order/Trade → update state. Contract metadata is injected synchronously via `load_contracts` or `register_contract`; it does not go through the event queue. Strategies and Hedge submit Intents only via RuntimeAPI.

**Events** — where they come from and what they drive:

| Type | Meaning | Source | Drives |
|------|---------|--------|--------|
| **Snapshot** | Portfolio prices and Greeks at a point in time | Backtest precomputed, live market injection | Portfolio state updates; unified view for strategies and risk engines |
| **Timer** | Clock/periodic trigger | Backtest each step, live timer thread | Strategy, position, hedge, and execution logic |
| **Order** | Order status update | Backtest matching, IbGateway fill | Order lifecycle observation; holdings and strategy state updates |
| **Trade** | Fill report | Backtest matching, IbGateway fill | Holdings, PnL, and risk metrics updates |

**Intents** — how they are executed in each runtime:

| Type | Meaning | Backtest | Live |
|------|---------|----------|------|
| **OrderRequest** | Order submission request | Backtest matching logic consumes in subsequent steps; order/trade state updated | Sent via IbGateway; callbacks become Order/Trade events |
| **CancelRequest** | Cancel request | Internal order state updated to control matching | Sent via IbGateway; result reflected in order state |
| **LogData** | Log | LogEngine filters by level; uniform format for replay/analysis | Same format; real-time monitoring and debugging |

**Backtest vs Live — event loop shape**:

| Dimension | Backtest | Live |
|-----------|----------|------|
| Time and event source | Timesteps generate Snapshot / Order / Trade / Timer in fixed order | Real clock + external systems: timer thread, market data, gateway callbacks |
| EventEngine | Single-threaded, sync; all events for a step consumed in one context | Queue + worker thread; producers enqueue, worker consumes; separate timer thread |
| Emphasis | Determinism, reproducibility, replayability, result statistics | Real-time responsiveness, external visibility, monitoring, fault tolerance |

### 1.5 RuntimeAPI

Capabilities injected by MainEngine into OptionStrategyEngine; strategies access the environment only through this API. Core does not hold MainEngine or IEventEngine; required capabilities are passed by caller or injected via RuntimeAPI.

| Group | Responsibility |
|-------|----------------|
| **ExecutionAPI** | Submit order, cancel, order/trade query, active order tracking |
| **PortfolioAPI** | Portfolio/contract view, strategy holding lifecycle |
| **SystemAPI** | Logging, helper engines (hedge, combo builder), strategy event push |

### 1.6 Portfolio and Snapshot

| Type | Description |
|------|-------------|
| **PortfolioData** | Top-level portfolio structure; `option_apply_order_` fixes option pointer order, one-to-one with Snapshot vector |
| **PortfolioSnapshot** | Compact snapshot; `apply_frame(snapshot)` writes prices and Greeks back into OptionData and UnderlyingData in the portfolio |
| **StrategyHolding** | One per strategy; contains underlying position and option positions (single-leg and multi-leg unified in optionPositions) and PnL, Greeks summary |

---

## 2. Layered Design

---

### 2.1 Domain Core

Pure business logic: receives Events (Timer, Snapshot, Order, Trade), updates internal state, and emits Intents (order, cancel, log). Contains strategy implementations, position tracking, hedging logic, combo building, execution caching, and logging—all without direct order submission, database access, or gateway calls. Environment access is via RuntimeAPI injection only.

| Component | Responsibility | Boundary |
|-----------|----------------|----------|
| **OptionStrategyEngine** | Strategy instance management and lifecycle (on_init / on_start / on_stop / on_timer); expose RuntimeAPI to strategies; handle Order/Trade events and call strategy on_order / on_trade | Depends only on RuntimeAPI; no direct dependency on MainEngine or EventEngine |
| **PositionEngine** | Maintain strategy holdings (StrategyHolding); update positions from Order/Trade; refresh summary metrics (update_metrics) from portfolio | Caller passes get_portfolio, portfolio, etc.; no execution callbacks |
| **HedgeEngine** | Centralized delta hedging logic; produces orders, cancels, logs from read-only params (portfolio, holding, active orders) | Read-only input; Runtime executes the output via send_order / cancel_order / put_log_intent |
| **ComboBuilderEngine** | Generate standardized Legs and combo signatures from ComboType and option data | Pure function style; get_contract passed by caller |
| **LogEngine** | Consume LogIntent; filter by level and output to sink | Single sink; output level controlled by LogEngine |
| **ExecutionEngine** | Central cache for orders and trades; maintain order-to-strategy mapping; encapsulate order submission (accept strategy name + OrderRequest, call runtime-injected send_impl) | Strategies and MainEngine interact via RuntimeAPI.execution; no direct container access |
| **Strategy Layer** | Implement concrete strategy logic (derive OptionStrategyTemplate); read portfolio/holdings in on_timer_logic, etc.; produce order/cancel/log intents | Access environment only via RuntimeAPI; StrategyRegistry maintains class name → factory |

### 2.2 Runtime

Orchestrates the event loop: sources data and clock, dispatches events to Core in fixed order, executes Intents (sends orders/cancels to gateway or backtest matcher), and turns execution results (fills, status updates) back into Events for Core. Backtest runtime drives timesteps synchronously; live runtime uses a queue and worker thread, with external market data and gateway callbacks feeding events.

| Component | Responsibility | Boundary |
|-----------|----------------|----------|
| **MainEngine** | Hold engine instances; provide send_order, cancel_order, put_log_intent, get_portfolio, get_contract, get_holding, etc.; assemble RuntimeAPI and inject into OptionStrategyEngine | Does not contain "dispatch order by event type" logic; put_event forwards to EventEngine |
| **EventEngine** | Receive events; dispatch by event type in fixed order (dispatch_snapshot, dispatch_timer, dispatch_order, dispatch_trade); execute intents via MainEngine | Does not hold engine instances; accesses via MainEngine accessors. Backtest: sync dispatch; live: queue + worker thread + timer thread |
| **BacktestEngine** | Backtest top-level controller; drive Snapshot → match → Timer per timestep; inject submit_order into MainEngine for matching | Single-threaded sync; no external network or database |
| **Live** | EventEngine uses queue and timer thread; MainEngine holds DatabaseEngine, MarketDataEngine, IbGateway; load_contracts at construction sets up portfolio structure; append_order / append_cancel to IbGateway; save_order_data / save_trade_data in dispatch_order / dispatch_trade | Contracts built by load_contracts callback directly calling market_data_engine_->process_option / process_underlying; no Contract event enqueued |
| **gRPC Service** | Hold MainEngine*; expose EngineService (GetStatus, ListStrategies, AddStrategy, StreamStrategyUpdates, etc.); RPCs call MainEngine or OptionStrategyEngine methods directly | Wraps existing capabilities only; no new domain logic |

### 2.3 Infrastructure

Data sources, persistence, and gateway. Provides market data, contract/order/trade storage, and broker connectivity.

| Component | Responsibility | Boundary |
|-----------|----------------|----------|
| **BacktestDataEngine** | Load historical data from parquet; build PortfolioData for backtest; precompute PortfolioSnapshot per frame; provide iter_timesteps and get_precomputed_snapshot | Backtest portfolio structure built at load_parquet; runtime only updates prices/Greeks via apply_frame |
| **MarketDataEngine** | Build contracts_, portfolios_ via load_contracts callback process_option / process_underlying; apply_frame available after finalize_all_chains; market data injected via inject_tradier_chain building Snapshot and put_event | Does not produce market data internally; data comes from external or inject-built Snapshot |
| **DatabaseEngine** | PostgreSQL; load_contracts iterates in fixed order (option then equity), calls apply_option / apply_underlying for each ContractData; save_order_data / save_trade_data called in dispatch_order / dispatch_trade | load_contracts does not put_event; callbacks directly build portfolio structure |
| **IbGateway** | Wrap IB TWS connection; send_order / cancel_order; order/fill reports fed back via main_engine->put_event(Order/Trade) | process_timer_event for periodic TWS message queue consumption |

---

### 2.4 Utilities

Shared data models, event types, and engine base classes. Defines Event/EventType/EventPayload, OrderData/TradeData/ContractData/PortfolioSnapshot/StrategyHolding, PortfolioData/ChainData/OptionData/UnderlyingData, and MainEngine/BaseEngine interfaces. Used by Core, Runtime, and Infrastructure.

| Category | Content |
|----------|---------|
| **Data and enums** | constant.hpp, object.hpp (OrderRequest, OrderData, TradeData, ContractData, PortfolioSnapshot, StrategyHolding, etc.), portfolio.hpp (PortfolioData, ChainData, OptionData, UnderlyingData) |
| **Event abstraction** | event.hpp (EventType, EventPayload, Event) |
| **Engine abstraction** | base_engine.hpp (MainEngine virtual interface, BaseEngine base class) |
