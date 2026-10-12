# AGENTS.md

How to work in this repository, for coding agents. Read it before a task; the design documents under `doc/` go deeper.

## What Optra is

One options trading and research system, in one repository:

| Part | Where | What it is |
|---|---|---|
| Engine | `engine/` | C++23, event-driven. Strategies take events (timer, snapshot, order, fill) and emit intents (order, cancel). One `core::Engine` runs a backtest (Parquet days, simulated fills) and live trading on Interactive Brokers (one process: IB market data, IB orders, SQLite, a gRPC service). |
| Numerics | `engine/quant/` | Pricing, Greeks, implied vol, Monte Carlo, trees, PDE, Heston, the screener: the engine links it; the server loads it through a C ABI (`engine/quant/analytics`, `libanalytics_c.so`). |
| Server | `app/server/` | One FastAPI app, one API under `/api`: the live engine through gRPC only, backtests through `entry_backtest`, the research tools (`/api/lab/...`) and the engine's (`/api/backtest/...`), each also at `/api/tools/{name}`, the run history (`/api/runs`), the daily check and report. |
| Page | `app/web/` | One Preact page, served by the server: Trading (Home, Account, Orders, Data) and Research (Backtest, Screener, Pricing, Multi-Leg, Risk, Numerics, Validation). |

Three ways across process boundaries, each for one kind of thing; a new feature uses the one that fits, never a fourth:
- **gRPC** to the live engine: it holds the trading state and must outlive the control plane.
- **C ABI** (`libanalytics_c.so`) for stateless numerics: JSON in, JSON out.
- **A subprocess** (`entry_backtest`, its stdout is the result JSON and nothing else) for backtests: parallel, killable, isolated.

Outside, a capability is a **tool**: declared once (name, a description written for a model, an input model whose every field is described, an example), reached over HTTP (its own route and `POST /api/tools/{name}`; `GET /api/tools` lists them) or from Python (`optra.tools.call`); its result comes in one envelope and is kept in the run history. Design the tool first; the page only calls it. No MCP, no separate CLI: the API is the interface for agents.

## Commands

```bash
./build.sh r                 # engine/build: entry_backtest, entry_live, entry_simulate, libanalytics_c.so (Release)
./build.sh r 0 g             # clean reconfigure, with the IB gateway (first time: fetches the TWS API)
ctest --test-dir engine/build                      # C++ tests (GoogleTest); no network, no database
(cd app/server && uv run --group dev --group databento pytest -q)   # ~15 min: real-data backtests
scripts/test_sanitize.sh address|thread            # ASan / TSan builds of the concurrency tests
run-clang-tidy -p engine/build -quiet engine/      # before finishing a C++ change
scripts/gen_proto.sh                               # after editing engine/entry/optra_engine.proto
(cd app/web && npm run build)                      # the page the server serves (app/web/dist)
./scripts/dev.sh                                   # server + page for development (PORT=8010 beside a deployed one)

./engine/build/entry_backtest --list-strategies    # every strategy and its parameters (JSON)
./engine/build/entry_backtest --files data/SPY/SPY-2025-03/*.parquet ComboStrategy leg1_ratio=1 leg2_ratio=1 leg2_right=1
```

Linux only, GCC 14 or newer, the Ubuntu packages (the apt line is in the README); `engine/CMakePresets.json` pins the toolchain and `build.sh` picks a preset. No data ships with the repository: synthetic days come from `scripts/data/synthetic_to_parquet.py` or `POST /api/data/synthetic` ([doc/data.md](doc/data.md)).

The IB paper-account tests (`engine/tests/integration`, label `ib_paper`) trade at market: only during market hours, only when asked, with `OPTRA_IB_PAPER_TESTS=1` (and `OPTRA_IB_PAPER_FILLS=1` for the ones that fill).

## Layout

```
engine/
  entry/       entry_backtest, entry_live; the gRPC API (optra_engine.proto, engine_grpc: GrpcLiveEngineService)
  core/        Engine (engine_main: the engines, the ports, what each event does) · the event loop · the two assemblies
    engines/   execution · position · hedge · risk · option strategy · log: no I/O
  strategy/    the template, the registry, factory/ (one .hpp/.cpp per strategy)
  model/       the data model: orders, fills, contracts, events, portfolios, symbols, combos
  adapters/    one folder per outside system: the venue port, ib/, backtest/, marketdata/, db/ (SQLite)
  utilities/   generic: base engine, logging, rings, pools, thread pool
  quant/       the numerics, and analytics/ (the research workflows and the C ABI)
  tests/       unit/, integration/ (IB paper), checks/ (by hand on data), bench/
app/server/    main.py; optra/api (routes), tools/ (the declared tools), services/ (domain, backtest days), infra/ (gRPC
               client, backtest runner, C ABI, run store), schemas/, proto/; tests/ (trading/, tools/, evals/)
app/web/       src/app.jsx (the shell), views/ (one per tab), ui/, charts/, lib/ (API client, store, history)
doc/           five documents for people: architecture, engine, server, research, data; README.zh.md (the Chinese
               README), architecture.html (the interactive diagram), images/
scripts/       build and development tools; data/ (Databento conversions, synthetic and recorded days, Deribit fixtures)
data/          backtest days, recordings, the live database (not in git)
```

## How the engine works

- **Events in, intents out.** `Engine::process_event` (`core/engine_main.cpp`) does what each event does, once for both modes: a snapshot updates its portfolio, then strategies' `on_snapshot`; the timer runs settlement, strategies, position metrics, hedges, in that order; an order update or a fill is booked (`book_order`, `book_trade`), then the strategy hears.
- **One writer.** Live, one event thread owns the trading state; the gateway, the market data and the log run on their own threads and only put events (fills and commands in an ordered queue, never dropped; a portfolio's newest snapshot). The event thread never waits on the outside: the database writer thread commits in batches.
- **The ports.** The core reaches out only through `RuntimeAPI` (`core/runtime_api.hpp`): orders and cancels to the venue, chain subscriptions, strategy updates, name checks. `core::backtest::MainEngine` and `core::live::MainEngine` only set the ports (event loop, clock, venue, storage, market data); booking, timer order, settlement and restore are written once.
- **A fill is one contract's.** A combo fills as its legs (the venue's port contract, `adapters/engine_gateway.hpp`); a combo position is keyed by its legs and its quantity comes from theirs. IB's own shapes (the BAG's execution, a one-leg combo) stay inside the IB gateway: [doc/engine.md](doc/engine.md#what-interactive-brokers-sends-back).
- **Deterministic booking.** `ExecutionEngine` merges each order update (a repeat or an out-of-date one changes nothing; an end is final) and holds an order's end until its fills are booked, so the holding never depends on arrival order.
- **Live restarts resume.** Every order and fill is saved; a restart rebooks them, checks the holdings against the broker before any strategy starts, and resumes the strategies that were running. Closing the engine does not stop strategies (`on_stop` may close positions); only the operator stops one.

## Common changes

- **A strategy**: a subclass of `strategy::OptionStrategyTemplate` in its own `.hpp/.cpp` under `strategy/factory/`, with a static `parameters()` (name, default, min, max, integer) read by `param<T>()`, and one `REGISTER_STRATEGY(Name)`. `on_timer_logic` runs once per bar in both modes: read the clock with `now()`, never by counting calls.
- **Something the page or a tool needs from the live engine**: a `MainEngine` operation (plain data), an RPC in the proto, `scripts/gen_proto.sh`, the server's client (`optra/infra`). The server never opens the engine's database.
- **A research capability**: a tool in `app/server/optra/tools/` (the engine's in `tools/engine.py`), its domain code in `services/`; a view in `app/web/src/views/` if a person needs it. No tool places an order.
- **Another broker or data vendor**: a subclass of `adapters::Gateway` (picked in `core::live::make_gateway`) or a `MarketDataFeed`.
- **A document**: one of the five under `doc/`, written for a person to read (what and why first, then how); a new topic goes into the document it belongs to, not a new file. Update values where they are.

## Conventions

- C++23, 4 spaces, at most 100 columns, no formatter (there is no `.clang-format`): write in the file's style.
- Names say literally what a thing is or does, in the style already there; no figures of speech in names, comments or logs. English in code, CMake, scripts and CI.
- Namespaces follow the folders: `utilities`, `model`, `quant` (`quant::analytics`), `core` (`core::live`, `core::backtest`), `strategy`, `adapters`, `entry`.
- `core/` holds engines only and no I/O; data structures go to `model/`, generic tools to `utilities/`. Add a method to the engine that owns the state rather than a new small type or file.
- Engines log through `write_log`, never `printf` or `std::cout`.
- Every library a target calls into is in its `target_link_libraries` (GNU ld resolves static libraries in link order).
- The proto is generated at build time: include `optra_engine.pb.h` by its bare name; edit the `.proto`, never the generated files.

## Guardrails

- **Paper only.** The engine trades the one account in `IB_ACCOUNT` and refuses a live one unless root names it in `/etc/optra/live-account`. Never weaken that, connect a gateway on your own, or loosen a risk limit unless asked.
- **No commit or push unless asked**, and no attribution trailers.
- **`python-mvp` stays hand-written**: no generated code goes into that branch.
- **Vendored code is fetched, not committed**: `engine/thirdparty/` (TWS API, Intel decimal library, LetsBeRational) comes from `scripts/fetch_ibjts.sh` and `scripts/fetch_lets_be_rational.sh`; don't assume it is there.
- **Verify before saying done**: the C++ tests in both builds (with and without IB), the server's tests, the page's build, and for engine changes the example strategies' backtest P&L against the last run.
