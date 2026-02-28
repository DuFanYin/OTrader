# Otrader Architecture

> This document describes the architecture of the Otrader C++ options engine: layers, responsibilities, and data flow.

---

```
Otrader/
├── entry_backtest.cpp                  # Backtest executable entry
├── entry_live.cpp                      # Live trading executable entry (no gRPC)
├── entry_live_grpc.cpp                 # Live trading + gRPC service entry
│
├── runtime/                            # Runtime: concentrates differences between backtest and live
│   ├── backtest/
│   │   ├── engine_backtest.{cpp,hpp}         # Backtest top-level controller
│   │   ├── engine_event.{cpp,hpp}            # Backtest event engine (synchronous dispatch)
│   │   └── engine_main.{cpp,hpp}             # Backtest MainEngine
│   │   
│   └── live/
│       ├── engine_event.{cpp,hpp}            # Live event engine (queue + worker threads)
│       ├── engine_main.{cpp,hpp}             # Live MainEngine
│       └── engine_grpc.{cpp,hpp}             # gRPC service implementation (holds MainEngine*)
│
├── infra/                                      # Infrastructure: data, persistence, gateways
│   ├── marketdata/
│   │   ├── engine_data_historical.{cpp,hpp}    # Backtest data engine (parquet → snapshot)
│   │   └── engine_data_tradier.{cpp,hpp}       # Live market data/combo engine (Contract + Snapshot)
│   ├── db/
│   │   └── engine_db_pg.{cpp,hpp}              # PostgreSQL contract/order/trade
│   └── gateway/
│       └── engine_gateway_ib.{cpp,hpp}         # IB TWS gateway
│
├── proto/
│   └── otrader_engine.proto                     # gRPC service and message definitions
│
├── core/                                        # Domain core: strategy engine and auxiliaries (no Context, caller passes parameters or uses RuntimeAPI)
│   ├── engine_option_strategy.{cpp,hpp}         # Unified strategy engine + RuntimeAPI
│   ├── engine_position.{cpp,hpp}                # Strategy position management
│   ├── engine_hedge.{cpp,hpp}                   # Hedge engine (produces orders/cancels/logs)
│   ├── engine_combo_builder.{cpp,hpp}           # Combo legs builder
│   ├── engine_log.{cpp,hpp}                     # Log engine (consumes LogIntent)
│   └── log_sink.hpp
│
├── strategy/                              # Strategy implementations and registry
│   ├── template.{cpp,hpp}                      # Strategy template base class
│   ├── high_frequency_momentum.{cpp,hpp}       # Example strategy
│   └── strategy_registry.{cpp,hpp}             # Strategy class name → factory
│
└── utilities/                             # Common data models and infrastructure

```


## 0. high‑level overview

### 0.1 Scope

Otrader is the C++ engine inside the FACTT platform. It is responsible for:

- representing market and portfolio state for options portfolios, and
- running **backtest** and **live** runtimes that drive the same domain core.

It does **not** try to be a data vendor, a portfolio‑optimisation UI, or a research notebook. It is an engine that other services and UIs can call.

### 0.2 Layers

The code under `Otrader/` is split into clear layers:

- **Utilities (`utilities/`)**
  - Shared data types: `ContractData`, `OrderData`, `TradeData`, `PortfolioData`, `PortfolioSnapshot`, `StrategyHolding`, events, enums.
  - Basic abstractions: `Event`, `IEventEngine`, `MainEngine` base, numerical helpers.

- **Domain core (`core/` + `strategy/`)**
  - `OptionStrategyEngine` and strategy template / registry.
  - `PositionEngine`, `HedgeEngine`, `ComboBuilderEngine`, `LogEngine`.
  - Core only sees:
    - typed **Events** coming in (Timer, Snapshot, Order, Trade, Contract),
    - and produces **Intents** going out (order requests, cancels, logs).
  - It does not know about threads, sockets, databases, or brokers.

- **Runtimes (`runtime/backtest/`, `runtime/live/`)**
  - Backtest: synchronous loop over timesteps (`BacktestEngine` + its `EventEngine` and `MainEngine`).
  - Live: event queue + timer thread + gateways + DB + optional gRPC service.
  - Runtimes are responsible for:
    - sourcing Events (from historical data, DB, broker),
    - executing Intents (order routing, persistence, logging),
    - feeding the core in a fixed order.

- **Infrastructure (`infra/`)**
  - Historical market data loader for backtests (`BacktestDataEngine`).
  - Market‑data / portfolio builder for live (`MarketDataEngine`).
  - Database engine (`DatabaseEngine`) and broker gateway (`IbGateway`).

The dependency direction is one‑way: `utilities → core/strategy → infra → runtime`.

### 0.3 Domain model

The engine models options trading as a small set of explicit concepts:

- **Contract → Portfolio → Holding → Snapshot → Event → Intent**
  - **Contracts** build up portfolios and option chains.
  - **Portfolios** and chains define structure once; they are reused across timesteps.
  - **Holdings** live per strategy and track positions (underlying, single‑leg, combos) and summaries (PnL, Greeks).
  - **Snapshots** are compact, time‑indexed market states applied via `PortfolioData::apply_frame(snapshot)`; they are the *only* way to change prices/Greeks.
  - **Events** carry snapshots, orders, trades, contracts, and timers into the core.
  - **Intents** (orders, cancels, logs) are what the core emits back to the runtime.

This split between structure (contracts, portfolios) and time‑varying data (snapshots, events) makes historical replay deterministic and reproducible.

### 0.4 Event and scheduling model

- **Event → Intent boundary**
  - Core engines consume `Event`s and produce Intents.
  - Runtimes decide how to execute Intents (send to broker, persist, log).

- **Fixed dispatch order**
  - For a given event, the processing order across engines is fixed in code (e.g. Snapshot → portfolio update; Timer → strategy / positions / hedging; Order/Trade → positions / strategy callbacks).
  - There is no dynamic handler registration or middleware chain, which makes behaviour easier to reason about.

- **Backtest vs live**
  - Backtest:
    - single process, single event thread,
    - synchronous loop over timesteps: for each step, apply Snapshot, then emit Timer, then process resulting Order/Trade events.
  - Live:
    - event queue + worker thread + timer thread,
    - events arrive from gateways, DB, and external data feeds and are processed in arrival order.
  - Both use the same core: only event scheduling and I/O integration differ.

### 0.5 Execution and OMS

Execution is handled by a dedicated **ExecutionEngine** in `core/engine_execution.*`:

- tracks all orders and trades in one place,
- maintains mappings from `orderid` to strategies and active orders per strategy,
- exposes a small API (via `RuntimeAPI.execution`) to:
  - submit orders,
  - cancel orders,
  - query orders and active orders.

Strategies and runtimes do not mutate order containers directly; they go through this OMS layer.

### 0.6 Summary of data flows

- **Contracts and portfolios**
  - Backtest: `BacktestDataEngine` builds `PortfolioData` and chains from historical data.
  - Live: `DatabaseEngine` emits Contract events; `MarketDataEngine` builds `PortfolioData` and chains.

- **Snapshots and prices**
  - Backtest: `BacktestDataEngine` precomputes `PortfolioSnapshot`s for each timestep; the runtime feeds them as Snapshot events.
  - Live: external feeds or services create `PortfolioSnapshot`s and feed them as Snapshot events.

- **Orders and trades**
  - In both modes, strategies call `send_order` through `RuntimeAPI`.
  - Backtest: ExecutionEngine forwards to a deterministic matching function; resulting Order/Trade events are fed back into the core.
  - Live: ExecutionEngine forwards to `IbGateway`; broker callbacks become Order/Trade events.

The remaining sections of this document go into more detail (in Chinese) about each layer, engine, and data flow.

---

## 1. Components and responsibilities

### 1.1 Core engines

- **OptionStrategyEngine (`core/engine_option_strategy.*`)**
  - Manages strategy instances and lifecycle (`on_init`, `on_start`, `on_timer`, `on_stop`).
  - Exposes a `RuntimeAPI` to strategies (portfolio/contract/holding access, send/cancel order, logging).
  - Receives order/trade events to call strategy callbacks (`on_order`, `on_trade`).

- **PositionEngine (`core/engine_position.*`)**
  - Maintains `StrategyHolding` per strategy (underlying/option/combo positions + PnL/Greeks summary).
  - Consumes Order/Trade events to update positions, and recomputes metrics periodically.

- **HedgeEngine (`core/engine_hedge.*`)**
  - Implements shared hedging logic (e.g. delta hedging) across strategies.
  - Reads holdings and active orders, emits order/cancel/log Intents.

- **ComboBuilderEngine (`core/engine_combo_builder.*`)**
  - Builds standardised combo legs from high‑level combo types (e.g. iron condor) and option data.
  - Ensures a consistent mapping from strategy‑level combo requests to individual legs.

- **LogEngine (`core/engine_log.*`)**
  - Consumes log Intents, filters by log level, and writes to a single sink.

- **ExecutionEngine (`core/engine_execution.*`)**
  - Central OMS: caches all orders and trades, tracks active orders per strategy, and provides query APIs.
  - Is the only place that mutates order state; strategies and runtimes go through it via `RuntimeAPI.execution`.

### 1.2 Strategy layer (`strategy/`)

- **Strategy template (`strategy/template.*`)**
  - Base class for strategies; owns references to portfolio, underlying, holdings and helper engines.
  - Implements the standard lifecycle hooks and helper methods (e.g. `get_chain`, `close_all_strategy_positions`).

- **Strategy registry (`strategy/strategy_registry.*`)**
  - Maps strategy class names to factory functions.
  - Backtest and live runtimes use the registry to instantiate strategies by name.

Concrete strategies (e.g. high‑frequency momentum, IV mean‑revert, iron‑condor‑style) are thin wrappers on top of this template and the `RuntimeAPI`.

---

## 2. Runtimes

### 2.1 Backtest runtime (`runtime/backtest/`)

- **BacktestEngine**
  - Top‑level controller for a backtest run.
  - Uses `BacktestDataEngine` (from `infra/marketdata`) to:
    - load historical data,
    - build `PortfolioData` and contracts,
    - precompute `PortfolioSnapshot`s for each timestep.
  - For each timestep:
    - emits a Snapshot event (portfolio update),
    - emits a Timer event (strategy/positions/hedging),
    - executes pending orders via a deterministic matching function and emits resulting Order/Trade events.

- **Backtest MainEngine + EventEngine**
  - `MainEngine` owns the core engines and `ExecutionEngine`, and exposes the `RuntimeAPI`.
  - `EventEngine` applies a fixed dispatch order for each event type; there is no handler registration.
  - There is no event queue: events are processed synchronously in the backtest loop thread.

### 2.2 Live runtime (`runtime/live/`)

- **Live MainEngine**
  - Owns `OptionStrategyEngine`, `PositionEngine`, `HedgeEngine`, `ComboBuilderEngine`, `LogEngine`.
  - Owns infrastructure engines: `MarketDataEngine`, `DatabaseEngine`, `IbGateway`.
  - Assembles the `RuntimeAPI` and passes it into `OptionStrategyEngine`.

- **Live EventEngine**
  - Runs an event queue with a worker thread plus a timer thread.
  - Receives:
    - Timer events from its own timer thread,
    - Order/Trade/Contract events from `IbGateway` and `DatabaseEngine`,
    - Snapshot events from external data feeds.
  - Processes events in arrival order, using the same fixed dispatch structure as the backtest.

- **gRPC service (`runtime/live/engine_grpc.*`)**
  - Wraps `MainEngine` with a gRPC `EngineService`:
    - engine status and gateway connections,
    - listing/adding/removing strategies,
    - querying portfolios, orders, trades, and holdings,
    - streaming logs and strategy updates.

---

## 3. Infrastructure

### 3.1 Backtest data engine (`infra/marketdata/engine_data_historical.*`)

- Loads historical data (e.g. from Parquet) into memory.
- Builds `PortfolioData` and option chains from contract information.
- Precomputes a sequence of `PortfolioSnapshot`s, one per timestep.
- Captures the contract → portfolio → snapshot mapping used by the backtest runtime.

### 3.2 Market data engine (`infra/marketdata/engine_data_tradier.*`)

- Consumes Contract events from `DatabaseEngine` and builds `PortfolioData` and chains for live trading.
- Maintains a stable `option_apply_order_` so that snapshots can be applied efficiently.
- Does not generate prices itself; it updates structure and delegates price updates to `apply_frame(snapshot)`.

### 3.3 Database engine (`infra/db/engine_db_pg.*`)

- Loads contracts from PostgreSQL at startup and emits Contract events.
- Persists orders and trades whenever dispatching Order/Trade events in the live runtime.

### 3.4 Broker gateway (`infra/gateway/engine_gateway_ib.*`)

- Wraps the IB TWS API.
- Sends and cancels orders on behalf of the `ExecutionEngine`.
- Converts broker callbacks into Order/Trade/Contract events and passes them into the event queue.

---

## 4. Utilities and data contracts

Key shared types in `utilities/`:

- **Event model** (`event.hpp`)
  - `EventType` = Timer, Snapshot, Order, Trade, Contract.
  - `EventPayload` = variant of the corresponding data structures.
  - `Event` = type + payload; the only cross‑layer carrier for runtime events.

- **Portfolio model** (`portfolio.hpp`, `object.hpp`)
  - `PortfolioData`, `ChainData`, `OptionData`, `UnderlyingData`.
  - `PortfolioSnapshot` as the compact numeric snapshot for one timestep.

- **Order and position model**
  - `OrderRequest`, `OrderData`, `TradeData`, `CancelRequest`, `Leg`.
  - `BasePosition` and derived types for underlying/option/combo positions.
  - `StrategyHolding` as the aggregate per‑strategy view.

These types form the stable contract between all engines and runtimes.

---

## 5. Event and intent lifecycle (summary)

### 5.1 Backtest

1. **Initialisation**
   - `BacktestEngine` loads data via `BacktestDataEngine`, registers `PortfolioData` and contracts, and precomputes snapshots.
   - Strategies are created via `StrategyRegistry` and registered with `OptionStrategyEngine`.
2. **Per timestep**
   - Emit Snapshot event → `PortfolioData::apply_frame`.
   - Emit Timer event → strategy `on_timer`, position metrics update, hedging.
   - Strategies and hedgers emit order/cancel/log Intents via `RuntimeAPI`.
   - Orders are matched deterministically, producing Order/Trade events.
   - Order/Trade events update `OptionStrategyEngine` callbacks and `PositionEngine` holdings.

### 5.2 Live

1. **Startup**
   - `MainEngine` is created, `DatabaseEngine::load_contracts` emits Contract events, and `MarketDataEngine` builds portfolios.
   - `EventEngine` starts its worker and timer threads; `IbGateway` connects to TWS.
2. **Steady state**
   - Timer thread emits Timer events at a fixed interval.
   - `IbGateway` feeds Order/Trade/Contract events from broker callbacks.
   - External feeds emit Snapshot events with current prices and Greeks.
   - Worker thread processes events from the queue, dispatching them to the core and infrastructure.
   - Strategies run in `on_timer`, emitting Intents that go through `ExecutionEngine` and `IbGateway`.

In both modes, the domain core is driven solely by Events and Intents; all I/O and concurrency are handled at the runtime and infrastructure layers.

