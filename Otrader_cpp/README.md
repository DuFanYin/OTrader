## FACTT – Options Trading & Research Platform

> **For a more detailed architecture overview, see [`ARCHITECTURE_EN.md`](ARCHITECTURE_EN.md).**

FACTT is an options trading and research platform that combines:

- a **web frontend** for running and inspecting backtests, viewing PnL curves and logs,
- a **backend service layer** for orchestrating runs and exposing APIs,
- and **Otrader**, a C++20 engine for systematic, portfolio‑level options trading using a shared core for both backtest and live execution.

The sections below describe the Otrader engine component of the platform.

---

## Otrader engine capabilities

- **Backtest runtime**
  - Replay full option chains from historical datasets, with portfolio snapshots applied at each timestep. A reference implementation is provided using Parquet via Arrow/Parquet, but the data loading layer is replaceable.
  - Simulate fills on single‑leg and multi‑leg structures (straddles, iron butterflies, iron condors, custom combos) with explicit legs and ratios, using a deterministic execution model.
  - Track per‑strategy PnL, Greeks, and holdings over **hundreds of thousands of timesteps** across multi‑day SPXW‑scale datasets.

- **Live trading runtime**
  - Integrates with Interactive Brokers (IBJts) for real order routing and market data.
  - Maintains a consistent, in‑memory view of portfolios, chains, positions, and open orders.
  - Exposes a gRPC API so dashboards, services, and notebooks can:
    - monitor engine health,
    - list and control strategies,
    - inspect orders/trades,
    - stream logs and strategy updates.

- **Code‑first, template‑driven strategies**
  - Strategies are **plain C++20 classes** built on a small template base and registry — no proprietary DSL, no GUI rule editor.
  - A unified API lets strategies:
    - read portfolios and option chains,
    - inspect holdings and risk,
    - send/cancel both single‑leg and combo orders,
    - emit structured logs.
  - The same compiled strategy binary runs **unchanged** in backtest and live; only configuration and data sources differ.

- **Combo‑aware risk and positions**
  - Native understanding of multi‑leg positions (e.g. iron condors, iron butterflies), not just “synthetic” approximations.
  - A dedicated combo builder standardizes how leg directions and ratios are specified across backtest and live.
  - Position and hedging engines operate on portfolio holdings, enabling portfolio‑level risk views.

---

## Otrader engine architectural model

- **Domain model**
  - Options trading is modelled explicitly as `Contract → Portfolio → Holding → Snapshot → Event → Intent`.
  - Structural data (contracts and portfolios) is separated from time‑varying data (snapshots and events), and strategy behaviour is expressed as intents.

- **Event → Intent boundary**
  - The core engines consume typed `Event`s (snapshot, timer, order, trade, contract) and produce `Intent`s (order requests, cancels, logs).
  - Runtime components decide how to execute intents (e.g. send to broker, persist to DB), which keeps domain logic and side effects separated.

- **Snapshot as the only state update mechanism**
  - Portfolio state changes are applied through `PortfolioSnapshot` objects via a single `apply_frame` entry point.
  - There are no ad‑hoc per‑field updates, which makes replay deterministic and time‑indexed.

- **Fixed dispatch order**
  - Within a timestep, dispatch order across engines is fixed in code rather than driven by dynamic handler registration or middleware chains.
  - This makes behaviour easier to reason about and avoids ordering surprises from runtime configuration.

- **Execution engine as a separate OMS layer**
  - Order tracking, active order maps, and strategy‑to‑order attribution are handled by a dedicated execution engine.
  - Strategies and runtimes interact with a clean OMS interface instead of updating order state directly.

- **One‑way dependency direction**
  - The dependency graph is `utilities → core → runtime`: core does not depend on runtime, gateways, or databases.
  - This allows the same core to be reused by different runtimes (synchronous backtest loop, asynchronous live event queue).

- **Backtest vs live scheduling**
  - Backtests run a synchronous, deterministic loop over timesteps; live uses an event queue plus timer thread.
  - Both drive the same core by feeding events and consuming intents, differing only in scheduling and I/O.

- **Single‑threaded core, multi‑threaded runtime**
  - Core engine logic executes in a single thread per runtime instance to avoid fine‑grained locking.
  - Concurrency is handled in the runtimes around I/O, data loading, and external integrations.

- **Contract → Portfolio → Snapshot model**
  - Structural information (contracts, chains, portfolios) is established first and then reused.
  - Market data updates are expressed as snapshots that update prices and Greeks without changing structure, keeping the market model explicit.

---

## Scope

FACTT and the Otrader engine focus on:

- representing market state and portfolio holdings,
- driving backtest and live runtimes,
- and providing an API surface for strategies and external services.

They do not aim to be:

- a data vendor or historical data catalogue,
- a portfolio optimisation UI,
- or a notebook/signal‑research environment.

---

## Learn more

- **Architecture details**: see `ARCHITECTURE.md` for a deeper, code‑level walkthrough.
