# AGENT.md

Working guide for AI coding agents (Claude Code and others) in this repo. Read this before a task; it should get you productive without spelunking. Link out only for deep design.

## The project

OTrader is a C++20, event-driven options trading & research engine. The contract is **Events in → Intents out**: strategies and core engines consume `Event`s (Timer, Snapshot, Order, Trade) and produce `Intent`s (order, cancel, log). The **same strategy code runs in backtest and live** — only the runtime wiring changes:

- **Backtest** — single process, synchronous loop over precomputed Parquet snapshots. No network, no DB.
- **Live** — a single process: `MainEngine` hosts the market-data engine and the gateway in-process, feeding the event engine directly; a gRPC service exposes the engine. `app/` is the FastAPI backend + Next.js UI on top.

`main` is this C++ engine (agent-assisted). `python-mvp` is the original hand-written Python it grew from. Full design: [doc/en/architecture.md](doc/en/architecture.md).

## Commands

```bash
./build.sh r                      # build engine (Release) → Otrader/build/{entry_backtest,entry_system}
./build.sh r 0                    # clean reconfigure + rebuild
./build.sh r 0 g                  # ...WITH the IB gateway (needs Otrader/thirdparty/IBJts)
./scripts/test_gtest.sh           # C++ unit tests (GTest); no network/DB needed
./scripts/test_otrader.sh         # end-to-end backtest/live script tests
./scripts/clang.sh f              # clang-format (then `t` for clang-tidy)
(cd app/backend && uv sync)       # Python backend deps
./scripts/system_up.sh dev        # full dev stack (engine + backend + UI)

# run a backtest: <parquet day> <registered strategy>
./Otrader/build/entry_backtest data/SPXW/SPXW-2025-08/20250804.parquet StraddleTestStrategy
```

Toolchain is pinned by `Otrader/CMakePresets.json`; `build.sh` just selects the host preset (`macos` / `macos-gateway` / `linux`). Direct: `cmake --preset macos && cmake --build --preset macos`.

## Layout

```
Otrader/           the engine — the only buildable C++ target
  core/            pure logic, no I/O: strategy · position · hedge · execution · log
  runtime/         backtest/ (sync loop) · live/ (queues + gRPC + in-process market-data/gateway)
  infra/           marketdata/ (Tradier) · gateway/ (IbGateway | NullGateway, live_gateway.hpp) · db/ (PostgreSQL)
  strategy/        template.hpp + strategy_registry.cpp + factory/ (concrete strategies)
  utilities/       base_engine, portfolio, black_scholes, parquet_loader, ring buffers, combo
  entry/           entry_system.cpp + run_live.cpp (single live runtime) + entry_modes.hpp
  proto/           .proto sources (the .pb.* are generated at build time)
app/               backend/ (FastAPI + gRPC bridge, self-contained uv project) · frontend/ (Next.js)
doc/               design docs (cn/, en/)   data/  user data (see data/README.md)
scripts/           dev/test/lint helpers · data_prep/ (Databento DBN -> backtest Parquet)
```

## How the engine fits together

- **Dispatch order** (`runtime/*/engine_event.cpp`): Snapshot → Timer → Order → Trade. Snapshot updates the portfolio via `apply_frame`; Timer drives strategy/position/hedge; Order/Trade update execution + positions then fire strategy `on_order`/`on_trade`.
- **RuntimeAPI** (`core/runtime_api.hpp`) is how core reaches the outside world, in three groups: **ExecutionAPI** (send/cancel order, order & trade queries), **PortfolioAPI** (portfolio/contract views, holdings), **SystemAPI** (logging, hedge engine, strategy-event push). Core never calls infra directly — it goes through here.
- **A strategy** subclasses `strategy/template.hpp`: implement `on_init_logic` / `on_stop_logic` / `on_timer_logic` (required) and optionally `on_order` / `on_trade`; read state via the template's helpers, act by emitting order/cancel intents. Register with one `REGISTER_STRATEGY(Name)` in `strategy/strategy_registry.cpp`.
- **Live threading** (`runtime/live`): three EventEngine threads — main worker (`run`), timer (`run_timer`), strategy (`run_strategy`) — plus the market-data poll thread and the gateway pump thread, all in one process. In-process callbacks (`MainEngine` ctor) feed market-data snapshots and gateway order/trade events straight into `put_event` (no serialization/IPC). All strategies share one strategy thread. Thread/queue detail: [doc/en/engine/](doc/en/engine/).

## Conventions

- C++20. Run `scripts/clang.sh f` (format) and `t` (tidy) before finishing a C++ change.
- Engines derive from `utilities::BaseEngine`; log via `write_log`, never `printf`/`cout`.
- Keep `core/` pure — no direct I/O; new capabilities go through `RuntimeAPI`, not a direct infra include.
- Match surrounding style, naming, and comment density. Prefer small, surgical edits.
- Adding a strategy → template subclass + `REGISTER_STRATEGY`. Swapping the live gateway → a new class with `IbGateway`'s public surface, selected in `infra/gateway/live_gateway.hpp` (a plain `using LiveGateway = ...`, no virtual interface).

## Guardrails

- **Real-money risk.** Never wire code toward a live brokerage account, auto-connect a gateway, or loosen order precautions unless the user explicitly asks.
- **Keep `python-mvp` hand-authored.** Don't cross-port agent-generated code into that branch.
- **Generated & vendored are not in git.** Edit `.proto`, not the generated `.pb.*`. `Otrader/thirdparty/` (IBJts, IntelRDFP, lets_be_rational) is gitignored — a fresh clone can't build the engine until it's restored; don't assume it's present.
- **Don't commit unless asked.** This repo does not use `Co-Authored-By` trailers.

## Gotchas that will actually bite

- **Single libc++.** Every dep links Apple's `/usr/lib/libc++`; the build must too. Forcing Homebrew LLVM's libc++ makes cross-library `catch(std::exception&)` silently miss → `std::terminate`. Don't reintroduce libc++ link flags. CMakeLists hardcode no compiler/prefix — deps resolve via `find_package`/`find_library` against the preset's `CMAKE_PREFIX_PATH`.
- **Proto is build-time generated.** Consumers include the generated headers by bare name (`#include "zmq_messages.pb.h"`), resolved through `otrader_proto`'s PUBLIC include dir — never a `../../proto/...` path. A `brew upgrade protobuf grpc` self-heals on the next configure.
- **IB is compile-time optional, via a typedef.** `infra/gateway/live_gateway.hpp` aliases `LiveGateway` to `IbGateway` when `BUILD_GATEWAY=ON` (`OTRADER_WITH_IB`, links IBJts) or `NullGateway` otherwise (logs + rejects orders, zero IB dependency). Default builds (`./build.sh r`) get NullGateway and need no IBJts; `./build.sh r 0 g` builds the real IB gateway and errors at configure if IBJts is absent. No `#ifdef` scattered in call sites — only the two gateway `.cpp`s and the alias header differ.
- **Live needs PostgreSQL + a Tradier token; backtest needs neither.** Live also needs a market-data source and gateway you supply — see the README's "What you provide" and [data/README.md](data/README.md).
