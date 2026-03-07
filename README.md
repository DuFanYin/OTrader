# OTrader

> **Note:** For showcase/demo only. Some components are intentionally omitted. No commit history. 

Strategy-oriented options trading and research: backtest, live via IB, web UI for control and inspection.

---

**Engine:** C++20, backtest + live dual mode. Event-driven: Events (Timer, Snapshot, Order, Trade) in → Intents (order, cancel, log) out.

**Layers**

- **Core:** Strategy, position, hedge, combo builder. Pure logic only; no direct I/O. Access to data and execution is via RuntimeAPI injection.
- **Runtime:** Event loop + orchestrator. Owns the event dispatch order and intent-handling call chain. In backtest the loop is synchronous; in live it uses a queue and worker thread.
- **Infra:** Supplies data and execution to Runtime. Pluggable to support different historical/live data sources and database.

**Modes**

- **Backtest:** Load historical data into precomputed snapshots; Single-process sync loop (snapshot → match → timer) with no network or database.
- **Live:** Market data normalised into portfolio snapshots; orders and cancels go through IbGateway; gRPC exposes the engine for external control.

**Strategies:** Implemented in C++ from the provided templates and built-in helpers. The same strategy code runs in both backtest and live; only the runtime and infra wiring differ.

---

## v2 — C++ (upgrade / work in progress)

**Status:** next-gen / WIP  
**Stack:** C++20, FastAPI, Next.js, PostgreSQL, gRPC  
**Linters:** clang-tidy, clang-format

### Core Engine Architecture

👉 **[v2_cpp_WIP/cpp_engines/ARCHITECTURE_EN.md](v2_cpp_WIP/cpp_engines/ARCHITECTURE_EN.md)**

---

- **System overview:** [v2_cpp_WIP/README.md](v2_cpp_WIP/README.md)  
- **Backend (FastAPI) overview:** [v2_cpp_WIP/backend/README.md](v2_cpp_WIP/backend/README.md)  
- **Frontend (Next.js) overview:** [v2_cpp_WIP/frontend/README.md](v2_cpp_WIP/frontend/README.md)

### Codebase Statistics

| Language      | Files | Blank | Comment | Code  |
|---------------|-------|-------|---------|-------|
| C++           | 27    | 716   | 174     | 7119  |
| Python        | 28    | 570   | 362     | 2979  |
| TypeScript    | 13    | 153   | 17      | 2185  |
| C/C++ Header  | 32    | 479   | 244     | 2120  |
| **Total**     | 100   | 1918  | 797     | 14403 |

---

## v1 — Python (legacy implementation)

**Status:** Original Python implementation, kept for reference.  
**Stack:** Python, FastAPI, SQLite, Tradier, IB TWS  
**Linters:** mypy, ruff.



- **Start here:** **[v1_python/README.md](v1_python/README.md)**
- **More docs:** **[v1_python/doc](v1_python/doc)** (engine design, strategy guide, diagrams)

---

## License / Usage
For **learning and portfolio/demo only**. No commercial use, redistribution, or production integration without written permission.
