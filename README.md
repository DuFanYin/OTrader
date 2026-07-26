<div align="center">

# OTrader

**A C++20 event-driven options trading & research engine — one strategy codebase, backtest and live.**

[![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus&logoColor=white)](#)
[![Build: CMake](https://img.shields.io/badge/build-CMake%20presets-064F8C?logo=cmake&logoColor=white)](#build--run)
[![Broker: IBKR](https://img.shields.io/badge/live-Interactive%20Brokers-FF6600)](#)
[![UI: Next.js](https://img.shields.io/badge/UI-Next.js%2016-black?logo=nextdotjs)](#)
[![License: non-commercial](https://img.shields.io/badge/license-learning%20%2F%20portfolio-lightgrey)](#license)

**English** · [中文](doc/README.zh.md)

</div>

---

Most backtesters and live traders are two different codebases that quietly disagree. **OTrader is one engine**: strategies consume **Events** (Timer, Snapshot, Order, Trade) and emit **Intents** (order, cancel, log) — the *exact same* strategy code runs in a deterministic backtest and against a live broker. Only the runtime wiring underneath changes.

The core is **~11K lines of C++** (engine + strategy framework), excluding tests, generated protobuf, and example strategies.

## Highlights

- 🔁 **One strategy, two modes** — write once, run in backtest and live; no separate "live rewrite" to drift out of sync.
- 🧩 **Pure-logic core, injected I/O** — strategies/position/hedge never touch the network or DB directly; everything arrives through a `RuntimeAPI`, so the core is trivially unit-testable.
- ⚡ **Built for low latency** — lock-free SPSC/MPSC ring buffers, object pools, pointer-payload events and a zero-copy hot path; heavy compute runs on a persistent thread pool, not per-event threads.
- 🛰️ **Process-isolated live stack** — market-data and IB gateway run as **independent processes** over ZeroMQ IPC; a gRPC service exposes the engine for external control.
- 📈 **Options-native** — multi-leg combos, Black-Scholes IV/Greeks, and a strategy-level delta-hedging engine, all built in.
- 🖥️ **Full control surface** — FastAPI backend + Next.js UI to launch backtests, manage strategies, and inspect orders/trades.

## Architecture

```
          Events (Timer · Snapshot · Order · Trade)
                          │
                          ▼
   ┌───────────────────────────────────────────────┐
   │  Domain Core  (pure logic, no I/O)             │
   │  strategy · position · hedge · execution · log │
   └───────────────────────────────────────────────┘
                          │  Intents (order · cancel · log)
                          ▼
   ┌───────────────────────────────────────────────┐
   │  Runtime   dispatch loop + intent handling     │
   │   backtest: synchronous   │   live: queues     │
   └───────────────────────────────────────────────┘
        │                                   │
        ▼ backtest                          ▼ live (separate processes, ZMQ IPC)
   Parquet history                     Market-data  ·  IB gateway  ·  gRPC
```

Everything the core needs is **injected** via `RuntimeAPI` (data views, order submission, logging). Swap the infra — a different data source, a paper gateway, another database — without touching a line of strategy or core logic.

## Build & Run

```bash
# 1. Build the C++ engine (Release) → Otrader/build/entry_backtest, entry_system
./build.sh r

# 2. Run a backtest: <parquet day> <registered strategy>
./Otrader/build/entry_backtest data/SPXW/SPXW-2025-08/20250804.parquet StraddleTestStrategy

# 3. Full dev stack (engine + FastAPI backend + Next.js UI)
(cd app/backend && uv sync)
./scripts/system_up.sh dev
```

**Prerequisites (macOS):** Clang, CMake ≥ 3.21, and Homebrew `protobuf grpc zeromq apache-arrow libpqxx`; Node.js for the UI; [`uv`](https://github.com/astral-sh/uv) for the Python backend. The toolchain is pinned by [`Otrader/CMakePresets.json`](Otrader/CMakePresets.json).

> Backtest needs none of the live infra — no network, no database. `./build.sh r` and go.

## What you provide

This is an engine, not a turnkey product. Three pieces are deliberately not bundled:

- **Backtest data.** No data ships with the repo. The backtester reads per-day Parquet files with a fixed schema (the samples were cleaned from [Databento](https://databento.com/) DBN, but any source works). You write the cleaning step — see **[data/README.md](data/README.md)** for the schema and layout.
- **A live market-data source.** The built-in provider polls the **Tradier** API and needs *your own paid Tradier production token* (`TRADIER_TOKEN`; the sandbox won't do). Prefer another vendor? Write a provider that emits `PortfolioSnapshot`s into the engine — the interface is small and swappable.
- **A live execution gateway.** The built-in one wraps Interactive Brokers' TWS API (**IBJts**), a private SDK not redistributed here — you obtain and compile it yourself. Or implement your own gateway against the ZeroMQ message contract (it runs as a separate process; the engine only speaks ZMQ to it). Live also needs PostgreSQL.

## Built-in Strategies

Registered in [`strategy_registry.cpp`](Otrader/strategy/strategy_registry.cpp) — run any by name, or use them as templates for your own:

| Strategy | Idea |
|----------|------|
| `StraddleTestStrategy` | Long/short straddle reference implementation |
| `IvMeanRevertStrategy` | Implied-vol mean reversion |
| `IronCondorTestStrategy` | Defined-risk iron condor |
| `StraddleInventoryScalperStrategy` | Straddle inventory scalping |

Writing your own is a subclass of the strategy template + one `REGISTER_STRATEGY(...)` line — see the [strategy guide](doc/).

## Documentation

- **Architecture** — [English](doc/en/architecture.md) · [中文](doc/cn/architecture.md)
- **Engine deep-dives** (thread model, zero-copy data path, low-latency design) — [doc/en/engine/](doc/en/engine/)
- **Component overviews** — [engine](Otrader/README.md) · [backend](app/backend/README.md) · [frontend](app/frontend/README.md)

## Two Lineages

This repo carries two implementations on purpose:

- **`main`** — the C++20 engine (this README). An **agent-assisted** rewrite focused on latency, structure, and dual-mode design.
- **[`python-mvp`](../../tree/python-mvp)** — the original **hand-written** Python implementation the C++ engine grew out of. Kept intact as the reference design.

## Layout

```
Otrader/   C++ engine — core · runtime · infra · strategy · utilities · entry
app/       backend (FastAPI, gRPC bridge) + frontend (Next.js UI)
doc/       design docs (cn/ + en/)
scripts/   dev launcher, test & lint helpers
```

## License

For **learning and portfolio/demo** purposes. No commercial use, redistribution, or production integration without written permission. Trading carries substantial risk; connecting to a live account is at your own risk.
