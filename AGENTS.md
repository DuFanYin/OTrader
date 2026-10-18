# AGENTS.md

A map of this repository for coding agents: where each thing is, which document explains it, how to build and check, and the few hard rules. The explanations live in `doc/`; read the one for the part you touch.

## Where things are

| Path | What it is | Read |
|---|---|---|
| `engine/core/` | the trading engine: `Main` (`main.*`), its event loop, and the engines, a folder each (`bookkeeping`, `risk`, `strategy`, `market_data`, `log`) beside the ports (`gateway`, `database`) | [doc/engine.md](doc/engine.md) |
| `engine/assembly/` | `Main` put together with its adapters: the live one (`live.*`, with the trading days) and the backtest (`backtest.*`: the replay, a day, many trials over many days) | [doc/engine.md](doc/engine.md) |
| `engine/adapters/` | the ports' implementations: IB, the backtest's venue, the simulated market, the Parquet days, SQLite | [doc/engine.md](doc/engine.md#how-the-engine-is-organised) |
| `engine/strategy/` | what a strategy is written against: the base class, its context (what it reads and does), the registry that loads strategy libraries | [doc/engine.md](doc/engine.md#writing-a-strategy) |
| `engine/model/`, `engine/utilities/` | the data model (contracts, orders, fills, combos, the portfolio: `Listing`, `Portfolio`, `PortfolioSnapshot`; a strategy's record, parameters declared with `Param`, values by day); generic pieces (queues, rings, pools, the thread pool, logging) | [doc/engine.md](doc/engine.md#how-the-engine-is-organised) |
| `engine/quant/` | the numerics (pricing, Greeks, implied vol, Monte Carlo, trees, PDE, Heston, the screener, the selection statistics: DSR, PBO, walk-forward) and its C ABI (`analytics/`) | [doc/research.md](doc/research.md#the-methods) |
| `engine/entry/` | the programs (`entry_backtest`, `entry_live`, `entry_simulate`) and the gRPC API (`optra_engine.proto`) | |
| `engine/tests/` | `unit/` mirrors `engine/`; `integration/` (IB paper); `checks/` (on data); `bench/` | [doc/latency.md](doc/latency.md) |
| `strategy/` | strategies, a library per `.cpp`: `demo/` the repository's (the only one in git); `research/`, `paper/`, `live/`, `configs/` the owner's and the agents' | [doc/engine.md](doc/engine.md#writing-a-strategy) |
| `app/server/optra/` | the server: `api/` routes, `tools/` the declared tools, `research/` experiments, ledger and jobs, `services/`, `infra/` (gRPC client, backtest runner, C ABI) | [doc/server.md](doc/server.md) |
| `app/web/src/` | the page: `app.jsx` the shell, `views/` one per tab, `ui/`, `charts/`, `lib/` | [doc/server.md](doc/server.md#the-page) |
| `scripts/` | `bench.sh` (latency benchmarks, a fixed clock), `dev.sh` (the stack for development), `fetch.sh` (third-party code), `test_sanitize.sh`, `backtest_sample.py` and `page_samples.py` (the pages' samples, made again); `data/` (an example conversion into backtest days, the live engine's quote recorder) | [doc/data.md](doc/data.md) |
| `doc/` | for people: architecture, engine, latency, backtesting, server, research, data; the Chinese README; `architecture.html` (the diagrams) | |
| `data/`, `local/` | backtest days and databases; the owner's notes (neither in git) | |

## Build, run, check

```bash
./build.sh r                      # engine/build: the programs, libanalytics_c.so (Release); g adds the IB gateway
./build.sh s                      # the strategy libraries only
./build.sh p                      # the server's Python stubs, after editing engine/entry/optra_engine.proto
ctest --test-dir engine/build     # C++ tests
(cd app/server && uv run --group dev pytest -q)   # server tests, minutes: in the background
(cd app/web && npm run build)     # the page the server serves
./scripts/dev.sh                  # engine, server and page together for development
./engine/build/entry_backtest --list-strategies
```

Linux, GCC 14 or newer (the apt line is in the README). No data ships with the repository: `python -m optra.data.synthetic_days` (in `app/server`) makes days; `scripts/data/example_to_parquet.py` shows how to convert your own ([doc/data.md](doc/data.md)).

## Where a change goes

- **A strategy**: first a setting of `ComboStrategy` (legs as rules); a new class in `strategy/research/` only for what its parameters cannot say.
- **An experiment**: a config in `strategy/configs/`, run by the `backtest` tool; counted in the ledger before it runs ([doc/server.md](doc/server.md#experiments)).
- **Something the page or an agent needs**: a tool in `app/server/optra/tools/` first, the page only calls it; from the live engine, an RPC in the proto ([doc/research.md](doc/research.md#for-agents)).
- **Another broker, data vendor or database**: an adapter behind `core::Gateway`, `core::MarketData` or `core::Database`.
- **A research question**: read what the engine already reports (per-trade attribution, the ledger's days, `replay_trial`) before writing a script; a question that comes back becomes a parameter or an engine feature.

## Rules

- **What approves, installs or changes what trades is a person's**, never an agent tool's (`optra/infra/access.py`).
- **Code**: C++23, 4 spaces, 100 columns, no formatter; names say literally what a thing does; English in code; minimal comments (why, not what); engines log through `write_log`.
- **Third-party code is fetched, not committed** (`scripts/fetch.sh`, into `engine/thirdparty/`).
- **Done means checked**: the C++ tests, the server's tests, the page's build; for an engine change, the example strategies' backtests against the last run.
