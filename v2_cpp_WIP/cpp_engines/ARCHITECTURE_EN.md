# Otrader Architecture

C++20 options portfolio trading engine. Backtest and live share Domain Core; runtime differs by data source, clock, and execution path.

---

## 0. File Tree

```
Otrader/
├── entry_backtest.cpp
├── entry_live.cpp
├── entry_live_grpc.cpp
├── entry_gateway.cpp                    # Gateway process (ZMQ REP/PUB)
├── entry_market_data.cpp                # Market Data process (ZMQ REP/PUB)
│
├── runtime/
│   ├── backtest/
│   │   ├── engine_backtest.{cpp,hpp}
│   │   ├── engine_event.{cpp,hpp}       # Sync dispatch
│   │   └── engine_main.{cpp,hpp}
│   │
│   └── live/
│       ├── engine_event.{cpp,hpp}       # Queue + main worker + strategy worker + timer
│       ├── engine_main.{cpp,hpp}
│       ├── engine_grpc.{cpp,hpp}
│       ├── gateway_client.{cpp,hpp}     # ZMQ REQ + SUB → entry_gateway
│       └── market_data_client.{cpp,hpp} # ZMQ REQ + SUB → entry_market_data
│
├── infra/
│   ├── marketdata/
│   │   ├── engine_data_historical.{cpp,hpp}  # Parquet → snapshot (backtest)
│   │   ├── engine_data_tradier.{cpp,hpp}      # Tradier + PortfolioStructure (live)
│   │   └── zmq_marketdata_schema.{cpp,hpp}   # Snapshot protobuf, REQ/REP commands
│   ├── db/
│   │   └── engine_db_pg.{cpp,hpp}
│   └── gateway/
│       ├── engine_gateway_ib.{cpp,hpp}       # IB TWS (used by entry_gateway)
│       └── zmq_gateway_schema.{cpp,hpp}      # Order/Trade protobuf, REQ/REP commands
│
├── core/
│   ├── portfolio_structure.{cpp,hpp}    # portfolios + contracts (backtest + live)
│   ├── engine_execution.{cpp,hpp}
│   ├── engine_option_strategy.{cpp,hpp}
│   ├── engine_position.{cpp,hpp}
│   ├── engine_hedge.{cpp,hpp}
│   ├── engine_combo_builder.{cpp,hpp}
│   └── engine_log.{cpp,hpp}
│
├── strategy/
│   ├── factory/
│   ├── template.{cpp,hpp}
│   └── strategy_registry.{cpp,hpp}
│
├── proto/
│   ├── otrader_engine.proto
│   └── zmq_messages.proto               # ZMQ Order/Trade/Snapshot/Request/Response
│
├── tests/
├── thirdparty/
└── utilities/
    ├── event.hpp
    ├── object.hpp
    ├── portfolio.hpp
    ├── base_engine.hpp
    ├── constant.hpp
    ├── datetime_serialize.hpp
    └── ...
```

---

## 1. Process Topology (Live)

```
        TWS ◄───┌──────────────────────────────────────────────────────────────────────────┐
                │  entry_gateway (process)                                                 │
                │  ┌─────────────┐  REP :5555   ┌─────────────┐  PUB :5556                 │
                │  │ IbGateway   │◄────────────►│ ZMQ REP     │────────────► Order/Trade   │
                │  │ (TWS API)   │   commands   │ (connect,   │                            │
                │  └──────┬──────┘              │  send_order)│                            │
                └─────────┼─────────────────────┴─────────────┴────────────────────────────┘
                          │
    ┌─────────────────────┼─────────────────────────────────────────────────────────────────┐
    │  entry_market_data  │                                                                 │
    │  DB/Tradier ◄──   ┌─────────────┐  REP :5557   ┌─────────────┐  PUB :5558             │
    │  ┌─────────────┐  │ MarketData  │◄────────────►│ ZMQ REP     │────────────► Snapshot  │
    │  │ DatabaseEng │─►│ Engine      │   commands   │ (start,     │                        │
    │  │ load_       │  │+Portfolio   │              │  subscribe) │                        │
    │  └─────────────┘  └──────┬──────┘              └─────────────┘                        │
    └──────────────────────────┼────────────────────────────────────────────────────────────┘
                               │
         Snapshot (5558)       │     Order/Trade (5556)
                               │
    ┌──────────────────────────┼─────────────────────────────────────────────────────────────┐
    │  entry_live_grpc         │                                                             │
    │  ┌──────────────────┐    │   ┌─────────────┐                                           │
    │  │ MarketDataClient │◄───┘   └──► ┌──────────────────┐                                 │
    │  │ SUB←5558 REQ→5557│             │ GatewayClient    │                                 │
    │  └────────┬─────────┘             │ SUB←5556 REQ→5555│                                 │
    │           │                       └────────┬─────────┘                                 │
    │           └─────────────┬──────────────────┘                                           │
    │                         ▼                                                              │
    │  ┌─────────────────────────────────────────────────────────────────────────────────┐   │
    │  │ MainEngine                                                                      │   │
    │  │   EventEngine ──► dispatch_snapshot/order/trade/timer                           │   │
    │  │   PortfolioStructure  OptionStrategyEngine  PositionEngine  ExecutionEngine     │   │
    │  │   HedgeEngine  LogEngine  ComboBuilderEngine                                    │   │
    │  └────────────────────────────────────────────┬────────────────────────────────────┘   │
    │                                               │ gRPC :50051                            │
    └───────────────────────────────────────────────┼────────────────────────────────────────┘
                                                    ▼
                                  Backend (FastAPI) / Frontend (Next.js)
```

---

## 1b. ZMQ Message Flow

```
  Runtime (entry_live_grpc)         entry_gateway           entry_market_data

  GatewayClient                     REP :5555               REP :5557
  ┌─────────────┐                   ┌─────────────┐         ┌─────────────┐
  │ REQ connect │─────────────────►│             │         │             │
  │ REQ send_   │◄─────────────────│  response   │         │  response   │
  │   order     │                   │             │         │             │
  └──────┬──────┘                   └──────┬──────┘         └──────┬──────┘
         │                                 │                       │
  SUB :5556                          PUB :5556                PUB :5558
         │                                 │                       │
         │◄──── Order/Trade (topic+payload) ─┤                       │
         │                                 │                       │
  MarketDataClient                         │                       │
  ┌─────────────┐                          │         Snapshot      │
  │ REQ start   │──────────────────────────────────── (topic+     │
  │ REQ sub_    │◄────────────────────────────────────  payload)   │
  │   chains   │                          │                       │
  └──────┬──────┘                          │                       │
         │                                 │                       │
  SUB :5558                                │                       │
         │◄──────────────────────────────── Snapshot ─────────────┘
         ▼
  MainEngine.put_event(Snapshot/Order/Trade)
```

---

## 2. ZMQ Addresses & Commands

| Process | REP (bind) | PUB (bind) | Env |
|---------|------------|------------|-----|
| entry_gateway | tcp://127.0.0.1:5555 | tcp://127.0.0.1:5556 | GATEWAY_REP_ADDR, GATEWAY_PUB_ADDR |
| entry_market_data | tcp://127.0.0.1:5557 | tcp://127.0.0.1:5558 | MARKETDATA_REP_ADDR, MARKETDATA_PUB_ADDR |

| Gateway REP commands | Payload |
|----------------------|---------|
| connect | ZmqConnectPayload |
| disconnect | — |
| send_order | OrderRequest |
| cancel_order | CancelRequest |
| query_account | — |
| query_position | — |

| Gateway PUB topics | Payload |
|--------------------|---------|
| order | OrderData |
| trade | TradeData |

| MarketData REP commands | Payload |
|-------------------------|---------|
| start | — |
| stop | — |
| subscribe_chains | ZmqSubscribeChainsPayload |
| unsubscribe_chains | ZmqUnsubscribeChainsPayload |

| MarketData PUB topics | Payload |
|-----------------------|---------|
| snapshot | PortfolioSnapshot |

---

## 3. Startup Order (system_up.sh)

| # | Process | Binary |
|---|---------|--------|
| 1 | Gateway | entry_gateway |
| 2 | Market Data | entry_market_data |
| 3 | Runtime | entry_live_grpc |
| 4 | Backend | Python FastAPI |
| 5 | Frontend | Next.js |

---

## 4. Mode Summary

**Backtest (single process):**

| Mode | Entry | Data | Gateway | Output |
|------|-------|------|---------|--------|
| Backtest | entry_backtest | BacktestDataEngine (parquet) | — | JSON stdout |
| Live (no gRPC) | entry_live | MarketDataClient (ZMQ) | GatewayClient (ZMQ) | In-process (entry_gateway, entry_market_data required) |
| Live (gRPC) | entry_live_grpc | MarketDataClient (ZMQ) | GatewayClient (ZMQ) | gRPC :50051 |

---

## 5. Core Concepts

| Concept | Description |
|---------|--------------|
| Event-In, Intent-Out | Inputs: Events (Snapshot, Timer, Order, Trade). Outputs: Intents (OrderRequest, CancelRequest, LogData) via RuntimeAPI. |
| Dispatch order | Snapshot → Order/Trade → Timer. Portfolio state updated before strategy logic. |
| Core isolation | Domain Core (OptionStrategyEngine, PositionEngine, etc.) has no direct MainEngine, DB, or gateway. Access via RuntimeAPI. |

---

## 6. Architecture Layers

```
  ┌───────────────────────────────────────────────────────────────────────────┐
  │  Domain Core                                                              │
  │  OptionStrategyEngine  PositionEngine  HedgeEngine  ComboBuilderEngine   │
  │  LogEngine  ExecutionEngine  Strategy implementations                    │
  └───────────────────────────────────┬───────────────────────────────────────┘
                                      │ RuntimeAPI only, no I/O
  ┌───────────────────────────────────┴───────────────────────────────────────┐
  │  Runtime                                                                  │
  │  BacktestEngine / EventEngine  MainEngine (backtest|live)  gRPC Service   │
  └───────────────────────────────────┬───────────────────────────────────────┘
                                      │ put_event, put_intent, accessors
  ┌───────────────────────────────────┴───────────────────────────────────────┐
  │  Infrastructure                                                           │
  │  BacktestDataEngine  MarketDataEngine  DatabaseEngine  IbGateway          │
  │  GatewayClient  MarketDataClient                                          │
  └───────────────────────────────────┬───────────────────────────────────────┘
                                      │
  ┌───────────────────────────────────┴───────────────────────────────────────┐
  │  Utilities  (event, object, portfolio, base_engine, constant, ...)         │
  └───────────────────────────────────────────────────────────────────────────┘
```

| Layer | Components |
|-------|------------|
| **Domain Core** | OptionStrategyEngine, PositionEngine, HedgeEngine, ComboBuilderEngine, LogEngine, ExecutionEngine, Strategy implementations |
| **Runtime** | BacktestEngine, EventEngine, MainEngine (backtest/live), gRPC Service |
| **Infrastructure** | BacktestDataEngine, MarketDataEngine, DatabaseEngine, IbGateway, GatewayClient, MarketDataClient |
| **Utilities** | event.hpp, object.hpp, portfolio.hpp, base_engine.hpp, constant.hpp, datetime_serialize.hpp |

---

## 7. MainEngine Ownership (Live)

```
MainEngine
├── EventEngine
├── LogEngine
├── DatabaseEngine
├── PortfolioStructure          ← portfolios + contracts (from load_contracts)
├── GatewayClient               ← ZMQ → entry_gateway
├── MarketDataClient            ← ZMQ → entry_market_data
├── ExecutionEngine
├── OptionStrategyEngine
├── PositionEngine
├── HedgeEngine
└── ComboBuilderEngine
```

---

## 8. PortfolioStructure

| Role | Backtest | Live |
|------|----------|------|
| Owner | MainEngine.portfolio_structure_ | MainEngine.portfolio_structure_ |
| Populated by | BacktestDataEngine.create_portfolio_data | db_engine.load_contracts → portfolio_structure.process_option/underlying (at MainEngine init) |
| Used by | BacktestDataEngine.portfolio_data() | MarketDataEngine (inherits, in entry_market_data); MainEngine.get_portfolio (in Runtime) |

---

## 9. EventEngine (Live) — Threads


| Thread | Queue | Dispatches to |
|--------|-------|---------------|
| Main worker | queue_ | dispatch_snapshot, dispatch_order, dispatch_trade, dispatch_timer (PositionEngine, HedgeEngine, push Timer → strategy_queue_) |
| Strategy worker | strategy_queue_ | OptionStrategyEngine (on_timer, process_order, process_trade) |
| Timer | — | Triggers dispatch_timer (1s interval) |

---

## 9.1 Object Pools (Performance / Allocation Control)

Otrader uses object pools to reduce allocations on hot paths.

| Pool | Component(s) | Stored as | Rule / Lifetime |
|------|--------------|----------|-----------------|
| `Event` | `runtime/live/engine_event.*` | `Event*` in `queue_` + `strategy_queue_` | Producer acquires/fills/pushes; consumer pops/processes/releases; `stop()` drains & releases. |
| `Event` | `runtime/backtest/engine_event.*` | acquired `Event*` (no queue) | `put_event` acquires/copies/dispatches/releases (synchronous). |
| `OrderData`, `TradeData` | `core/engine_execution.*` | `OrderData*`, `TradeData*` in maps | `get_order/get_trade` pointers are valid until entry removal or `ExecutionEngine::clear()`. |
| `LogData` | `core/engine_log.*` | pooled copy inside `process_log_intent` | Caller API unchanged; LogEngine acquires/copies/emits/releases. |
| `PortfolioSnapshot` | `infra/marketdata/engine_data_historical.*` | `PortfolioSnapshot*` in `snapshots_` | Backtest precompute: recompute releases old pointers and reuses memory. |

Implementation status and follow-ups: `docs/object_pool_candidates.md`.

---

## 10. Event Flow

```
  Event-In                                              Intent-Out
  ────────                                              ──────────

  Snapshot ──► MainEngine.put_event ──► queue_ ──► dispatch_snapshot ──► Portfolio.apply_frame
       ▲                                                                      │
       │  (BacktestDataEngine precomputed / MarketDataClient SUB)             │
       └──────────────────────────────────────────────────────────────────────┘

  Order   ──► MainEngine.put_event ──► queue_ ──► dispatch_order ──► ExecutionEngine, PositionEngine
       │                                  └──► strategy_queue_ ──► OptionStrategyEngine.process_order
       │  (Backtest matching / GatewayClient SUB)                                  │
       │                                                                           ▼
       │                                                                  send_order / cancel_order
       │                                                                  (GatewayClient.req_rep)

  Trade   ──► MainEngine.put_event ──► queue_ ──► dispatch_trade ──► ExecutionEngine, PositionEngine
       │                                  └──► strategy_queue_ ──► OptionStrategyEngine.process_trade
       │  (Backtest matching / GatewayClient SUB)

  Timer   ──► (Timer thread) put ──► queue_ ──► dispatch_timer ──► PositionEngine, HedgeEngine
       │                                  └──► strategy_queue_ ──► OptionStrategyEngine.on_timer
       │  (Backtest: per step)                                                    │
       │                                                                           ▼
       │                                                                  Intents (order/cancel/log)
```

| Event | Source | Sink |
|-------|--------|-----|
| Snapshot | BacktestDataEngine (precomputed) / MarketDataClient (ZMQ SUB) | Portfolio.apply_frame |
| Timer | Backtest step / Timer thread | OptionStrategyEngine.on_timer |
| Order | Backtest matching / GatewayClient (ZMQ SUB) | ExecutionEngine, PositionEngine, OptionStrategyEngine.process_order |
| Trade | Backtest matching / GatewayClient (ZMQ SUB) | ExecutionEngine, PositionEngine, OptionStrategyEngine.process_trade |

---

## 11. Intent Flow

| Intent | Backtest | Live |
|--------|----------|------|
| OrderRequest | BacktestEngine matching | GatewayClient.req_rep(send_order) |
| CancelRequest | BacktestEngine matching | GatewayClient.req_rep(cancel_order) |
| LogData | LogEngine | LogEngine |

---

## 12. Event-In, Intent-Out

**Dispatch order**: Snapshot → Order/Trade → Timer

**Events**: Snapshot, Timer, Order, Trade

**Intents**: OrderRequest, CancelRequest, LogData (via RuntimeAPI)

---

## 13. RuntimeAPI Groups

| Group | Capabilities |
|-------|--------------|
| ExecutionAPI | send_order, cancel_order, get_order, get_trade, active orders |
| PortfolioAPI | get_portfolio, get_contract, get_holding, subscribe_chains |
| SystemAPI | write_log, hedge_engine, combo_builder, strategy_event |

---

## 14. Data Types

| Type | Description |
|------|-------------|
| PortfolioData | option_apply_order_, OptionData, UnderlyingData |
| PortfolioSnapshot | bid/ask/last/delta/gamma/theta/vega/iv vectors |
| StrategyHolding | underlying position, optionPositions, PnL, Greeks |

---

## 15. Proto

| File | Contents |
|------|-----------|
| otrader_engine.proto | gRPC EngineService |
| zmq_messages.proto | ZmqOrderData, ZmqTradeData, ZmqOrderRequest, ZmqCancelRequest, ZmqConnectPayload, ZmqRequest, ZmqResponse, ZmqPortfolioSnapshot, ZmqSubscribeChainsPayload, ZmqUnsubscribeChainsPayload |
