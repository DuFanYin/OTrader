# Otrader — C++ Engine

The C++20 options engine at the heart of the project: an event-driven core (Timer/Snapshot/Order/Trade in → order/cancel/log intents out) shared by both the deterministic Parquet backtest and the live IB-TWS runtime. This directory is the engine only; the backend and frontend live one level up.

## Layout

```
core/         Domain logic: strategy, position, hedge, execution, log (no direct I/O)
runtime/      backtest/ (sync loop) · live/ (queue + gRPC + ZMQ clients) · main_engine_base
infra/        db/ (PostgreSQL) · gateway/ (IB TWS) · marketdata/ (Tradier)
strategy/     template + strategy_registry (REGISTER_STRATEGY) + factory/
utilities/    base_engine, portfolio, combo_builder, black_scholes, parquet_loader, ring buffers
proto/        .proto + generated protobuf/gRPC and ZMQ message code
tests/        GTest unit tests + non-framework backtest/live tests
entry_backtest.cpp   entry_system.cpp   (unified live entry: --mode=gateway|market|live|all)
```

## Build & run

Build from the repo root (see the top-level [README](../README.md)):

```bash
./build.sh r    # → Otrader/build/entry_backtest and Otrader/build/entry_system
./Otrader/build/entry_backtest <parquet> <StrategyName>
```

## Docs

- **Architecture:** [../doc/en/architecture.md](../doc/en/architecture.md) ([中文](../doc/cn/architecture.md))
- **Engine deep-dives** (thread model, zero-copy, low-latency, build units, testing): [../doc/](../doc/)
- **Backend / Frontend:** [../app/backend/README.md](../app/backend/README.md) · [../app/frontend/README.md](../app/frontend/README.md)
