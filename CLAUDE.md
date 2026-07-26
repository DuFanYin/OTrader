# CLAUDE.md

Guidance for Claude Code (and any AI assistant) working in this repository.

## What this is

OTrader — a C++20, event-driven options trading & research engine with backtest + live dual mode, a FastAPI backend, and a Next.js control UI. This is the **agent-assisted C++ rewrite** of the original hand-written Python implementation (which lives on the `python-mvp` branch).

**Engine model:** Events (Timer, Snapshot, Order, Trade) in → Intents (order, cancel, log) out. Core is pure logic; data/execution are injected via RuntimeAPI. Backtest runs a synchronous loop; live runs market-data and gateway as independent processes over ZeroMQ IPC, with gRPC exposing the engine.

## Layout

```
Otrader/           The C++ engine — the ONLY buildable engine (has CMakeLists)
  core/            Strategy, position, hedge, combo — pure logic, no I/O
  runtime/         backtest/ (sync loop) and live/ (queue + gRPC + ZMQ clients)
  infra/           db/ (PostgreSQL via libpqxx), gateway/ (IB TWS), marketdata/ (Tradier)
  proto/           otrader_engine.proto, zmq_messages.proto + generated .pb.* (checked in)
  strategy/        template + strategy_registry (REGISTER_STRATEGY)
  utilities/       base_engine, portfolio, black_scholes, parquet_loader, ring buffers, ...
  tests/           gtest unit tests
  thirdparty/      IBJts (IB TWS API), IntelRDFPMathLib, lets_be_rational — GITIGNORED
  build/           CMake build dir — gitignored
  entry_backtest.cpp   entry_system.cpp   (unified live entry: --mode=gateway|market|live|all)
backend/           FastAPI server (src/) bridging the engine over gRPC
frontend/          Next.js 16 + Tailwind v4 control UI
doc/               Design docs, language-partitioned: doc/cn/ and doc/en/
build.sh           C++ build (Release: ./build.sh r). Uses Otrader/build + Homebrew LLVM.
system_up.sh       Dev launcher (engine + backend + frontend)
run_backtest.py    Python backtest driver (imports backend.src.*)
pyproject.toml / uv.lock   Project Python deps (backend + backtest driver), managed with uv at repo root
```

## Build & run

```bash
./build.sh r           # Release build → Otrader/build/entry_backtest, entry_system
uv sync                # backend Python venv
./system_up.sh dev     # full dev stack

# backtest: <parquet> <registered-strategy>
./Otrader/build/entry_backtest data/SPXW/SPXW-2025-08/20250804.parquet StraddleTestStrategy
```

Registered strategies (see `Otrader/strategy/strategy_registry.cpp`): StraddleTestStrategy, IvMeanRevertStrategy, IronCondorTestStrategy, StraddleInventoryScalperStrategy.

## Toolchain gotchas (learned the hard way)

- **Build uses Homebrew LLVM/clang**, not Apple clang: `/opt/homebrew/opt/llvm/bin`. Needs cmake, protobuf, grpc, zlib from Homebrew.
- **Proto is generated at build time**, not checked in. `Otrader/proto/` holds only the `.proto` sources; `proto/CMakeLists.txt` runs `protoc` (+ grpc plugin) into the build dir, so a `brew upgrade protobuf grpc` self-heals on the next configure — no manual regeneration, no version-mismatch errors. Needs `protoc` + `grpc_cpp_plugin` on PATH (override via `-DPROTOC=` / `-DGRPC_CPP_PLUGIN=`). Consumers include the generated headers by bare name (e.g. `#include "zmq_messages.pb.h"`), resolved through `otrader_proto`'s PUBLIC include dir — never by a `../../proto/...` path.
- **IB is compile-time optional.** `entry_system` is a thin `main()` dispatcher; each mode lives in its own TU under `Otrader/entry/` (`run_live.cpp`, `run_market.cpp`, `run_gateway.cpp`). Only `run_gateway.cpp` touches IBJts, and it's compiled + `OTRADER_WITH_IB` defined only when `BUILD_GATEWAY=ON`. So live/market/backtest build with **no IBJts present** (`entry_system` then links no twsapi and rejects `--mode=gateway`/`--mode=all` at runtime with exit 1). When adding a mode source, wire it into `entry/entry_modes.hpp` + the `ENTRY_SOURCES` list in the root CMakeLists.
- **When you do build IB** (`BUILD_GATEWAY=ON`): IBJts must be rebuilt against the current protobuf; `build.sh` builds it into `Otrader/thirdparty/IBJts/build-apple/` only if the dylib is missing (delete to force). Its vendored CMake needs C++17, links `libbid.a` + absl-log, and needs `-DCMAKE_POLICY_VERSION_MINIMUM=3.5` on CMake 4.x. Note: `build.sh` still hard-requires `Otrader/thirdparty/IBJts` to exist — to build without IB, configure cmake directly with `-DBUILD_GATEWAY=OFF`.
- **Runtime needs `DYLD_LIBRARY_PATH`** to include `Otrader/thirdparty/IBJts/build-apple/lib` (only when IB is built).
- **Live/market modes require PostgreSQL** (default `dbname=trading`, or `DATABASE_URL`) and a Tradier token (`.env`). Backtest needs neither.
- **thirdparty/ is gitignored** (`lets_be_rational`, `IntelRDFPMathLib`, `IBJts`) — a fresh clone has none of it, and utilities won't even configure without `lets_be_rational`. Restore from a sibling checkout or the setup flow before building.

## Conventions

- C++20. Formatting/linting: `clang-format`, `clang-tidy`.
- Engines derive from `utilities::BaseEngine`; logging goes through `write_log`.
- Match the surrounding code's style, naming, and comment density for any change.
- Never wire code toward a live brokerage account or loosen order precautions unless the user explicitly asks — live trading is real-money risk.

## Note

The `python-mvp` branch is the hand-written Python original; keep that line hand-authored. This branch (`main`) is where the agent-assisted C++ work happens.
