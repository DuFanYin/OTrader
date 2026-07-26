# OTrader

> **Note:** For showcase/demo only. Some components are intentionally omitted.

Strategy-oriented options trading and research: backtest, live via IB, web UI for control and inspection.

> **🤖 Agent-assisted C++ rewrite.**
> This is the C++ implementation, developed with AI agent assistance. The original, hand-written Python version lives on the [`python-mvp`](../../tree/python-mvp) branch (hand-authored, no agent).

---

**Stack:** C++20, FastAPI, Next.js, PostgreSQL, gRPC, ZeroMQ
**Toolchain:** LLVM/clang, CMake; `clang-tidy`, `clang-format`
**Backend Python:** managed with `uv`

**Engine:** backtest + live dual mode. Event-driven: Events (Timer, Snapshot, Order, Trade) in → Intents (order, cancel, log) out.

**Layers**

- **Core:** Strategy, position, hedge, combo builder. Pure logic only; no direct I/O. Access to data and execution is via RuntimeAPI injection.
- **Runtime:** Event loop + orchestrator. Owns the event dispatch order and intent-handling call chain. In backtest the loop is synchronous; in live it uses a queue and worker thread.
- **Infra:** Supplies data and execution to Runtime. Pluggable to support different historical/live data sources and database.

**Modes**

- **Backtest:** Load historical Parquet data into precomputed snapshots; single-process sync loop (snapshot → match → timer) with no network or database.
- **Live:** Market data normalised into portfolio snapshots; orders and cancels go through IbGateway; gRPC exposes the engine for external control. Market-data and gateway run as independent processes over ZeroMQ IPC.

**Strategies:** Implemented in C++ from the provided templates and built-in helpers. The same strategy code runs in both backtest and live; only the runtime and infra wiring differ.

---

## Documentation

- **Core engine architecture:** [doc/en/architecture.md](doc/en/architecture.md) ([中文](doc/cn/architecture.md))
- **Low-latency design notes:** [doc/en/engine/lowLatencyEfforts.md](doc/en/engine/lowLatencyEfforts.md)
- **All design docs:** [doc/](doc/)
- **Engine overview:** [Otrader/README.md](Otrader/README.md)
- **Backend (FastAPI) overview:** [backend/README.md](backend/README.md)
- **Frontend (Next.js) overview:** [frontend/README.md](frontend/README.md)

## Build & Run

```bash
# Build the C++ engine (Release). Produces Otrader/build/entry_backtest and entry_system.
./build.sh r

# Run a backtest (parquet day + registered strategy name)
./Otrader/build/entry_backtest data/SPXW/SPXW-2025-08/20250804.parquet StraddleTestStrategy

# Backend deps (uv) + full dev stack (engine + FastAPI + Next.js)
uv sync
./system_up.sh dev
```

Prerequisites (macOS): Clang (Apple clang is fine), CMake ≥ 3.21, and Homebrew protobuf, gRPC, ZeroMQ, apache-arrow, libpqxx; Node.js for the frontend; `uv` for the Python backend. The toolchain is pinned by `Otrader/CMakePresets.json`. Live mode additionally needs PostgreSQL and IB TWS/Gateway + a Tradier token. See the linked docs for full setup.

---

## License / Usage
For **learning and portfolio/demo only**. No commercial use, redistribution, or production integration without written permission.
