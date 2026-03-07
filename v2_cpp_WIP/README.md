### OTrader v2: Options Strategy Research and Execution

A C++20 options engine with event-driven backtesting, live trading via IB TWS, and a FastAPI/Next.js control surface.

---

### Key Architecture Docs

- **C++ Engine (Otrader):** [cpp_engines/ARCHITECTURE_EN.md](cpp_engines/ARCHITECTURE_EN.md)  
- **Backend (FastAPI + gRPC + DB):** [backend/README.md](backend/README.md)  
- **Frontend (Next.js UI):** [frontend/README.md](frontend/README.md)

---


### Overview

FACTT is built around **Otrader**, a C++20 engine for multi-leg options portfolio trading, with a Python backend and a React/Next.js frontend.  
The system supports **deterministic backtesting** over Parquet historical data, **live trading** through Interactive Brokers TWS via gRPC, and a **web UI** for managing strategies, running backtests, and inspecting orders and trades.

---

### Core Capabilities

- **Unified options trading engine (Otrader)**:  
  Event-driven core that consumes Timer/Snapshot/Order/Trade events and produces intents for orders, cancels, and logs.

- **Shared domain core for backtest and live**:  
  Strategy, execution, position, hedge, combo-builder, and logging engines are reused across both modes; runtimes only differ in data sources and execution wiring.

- **Parquet-based backtesting**:  
  Historical data from Parquet is converted into portfolio snapshots and replayed in a deterministic loop that drives strategy logic and order lifecycle.

- **Live trading via IB TWS**:  
  A gateway engine wraps IB TWS for connectivity and order routing, feeding order/trade callbacks back into the event engine.

- **PostgreSQL-backed state**:  
  Contract metadata and order/trade history are persisted via a PostgreSQL database engine and exposed through backend APIs.

- **gRPC-based live engine control**:  
  The live C++ engine exposes status, strategy management, portfolio metadata, orders/trades, and logs over gRPC.

- **FastAPI backend bridge**:  
  HTTP APIs and WebSockets for the frontend, plus a bridge to the C++ backtest executable and live gRPC engine.

- **Backtest and strategy management UI**:  
  Next.js pages for configuring/running backtests and for listing, creating, and controlling live strategies.

- **Orders & trades and database UIs**:  
  Pages for querying historical orders/trades and inspecting contract tables, using the backend’s database endpoints.

---

### System Architecture

The system is organized into three main layers: the **Otrader engine** (C++), the **backend service** (Python/FastAPI), and the **frontend UI** (Next.js/React).

- **Otrader engine**  
  Domain core (strategies, positions, execution, hedging, logging), event engine, backtest/live runtimes, data engines, database integration, IB gateway, and entrypoints for backtest and live processes.

- **Backend service**  
  FastAPI app that exposes engine control, backtest, database, and log endpoints; bridges to the C++ backtest executable and gRPC live engine; and manages WebSockets and lightweight application state.

- **Frontend UI**  
  Next.js application: layout, status/navigation components, and pages for backtesting, live strategy management, orders/trades inspection, and database views, following the trading-desk UI spec.

These layers interact as: frontend → FastAPI backend (HTTP/WebSocket) → C++ engine (subprocess or gRPC) → external data sources (Parquet, PostgreSQL, IB TWS).

---

### Modularity & Customization

FACTT is designed to be modular: infra components can be swapped without changing the strategy/domain core.

- **Snapshot-driven pricing model**: portfolios receive `Snapshot` events and compute IV/Greeks internally on-the-fly.
- **Pluggable historical data engine**: can adapt to different historical formats as long as underlying + option prices are provided.
- **Pluggable market-data engine**: can integrate with different providers (REST, WebSocket, or other streaming transports).
- **Pluggable storage layer**: the database engine can be adapted to different persistence backends beyond PostgreSQL.

### Data Flow

Conceptually, data flows through the system as an event-driven pipeline:

- **Market data → snapshots**:  
  Backtest data is loaded from Parquet and precomputed into ordered portfolio snapshots; live data from market data engines and IB callbacks is turned into snapshots and injected as `Snapshot` events.

- **Snapshots/timers → strategy logic**:  
  The `EventEngine` dispatches `Snapshot` and `Timer` events, the domain core updates portfolio state, and strategies evaluate signals on the updated view.

- **Strategy logic → intents → execution**:  
  Strategies produce order/cancel/log intents; `MainEngine` and `ExecutionEngine` route these either to the backtest execution model or the live IB gateway.

- **Execution → orders/trades → positions**:  
  Fills and order updates become `Order` and `Trade` events; `PositionEngine` updates holdings and PnL/Greeks, and strategies receive order/trade callbacks.

- **Persistence → API/UI**:  
  Live orders/trades and contracts are stored in PostgreSQL; the backend exposes them over HTTP, and the frontend renders tables, filters, and logs.

---

### Backtesting Model

The backtesting model uses the backtest runtime and the historical data engine:

- **Event-driven, single-process simulation**:  
  A `BacktestEngine` iterates over precomputed snapshots, applies each snapshot to the portfolio, and drives the event engine with `Snapshot`, `Timer`, and resulting `Order`/`Trade` events in a fixed order.

- **Execution and lifecycle modeling**:  
  Strategy-generated order/cancel intents go through a backtest execution path that models fills using snapshot data, fee rate, slippage (bps), risk-free rate, and IV price mode (`mid`/`bid`/`ask`), while reusing the same `ExecutionEngine` and `PositionEngine` as live mode.

- **Position tracking, metrics, and outputs**:  
  Positions, PnL, and Greeks are maintained per strategy, and the backtest executable returns JSON with summary metrics, timestep metrics, per-day results, and chart data (rendered to SVG/PNG by the backend).

---

### Future Work

Given the current architecture, natural extensions include:

- **More robust execution & order management**: add more realistic execution rules (e.g., fill-or-kill) and richer order lifecycle modeling.
- **Production-grade historical preprocessing pipeline**: replace ad-hoc scripts with a reproducible pipeline (validation, normalization, metadata, caching).
- **Higher-resolution historical data**: evaluate feeds such as Databento SPX `CMBP-1`, `TCBBO`, `CBBO-1s` for improved intraday realism.
- **Market data engine migration**: move away from Tradier REST pull (no longer free) to a unified streaming provider (e.g., Databento live stream).
- **Latency & health observability**: add Prometheus metrics for end-to-end latency, engine health, gateway connectivity, and queue/backpressure signals.
- **Standardized symbol/config pipeline**: standardize the workflow to support different underlyings and option chains consistently (naming, calendars, contract metadata, and roll rules).