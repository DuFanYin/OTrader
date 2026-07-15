# Backend Architecture Overview

FastAPI backend for the frontend, providing:

- **Live gateway / market data status** (via C++ gRPC engine)
- **Strategy management** (list, start/stop, params, holdings)
- **Backtest jobs** (invokes C++ backtest executable)
- **System views and DB queries** (orders/trades, contract overview, etc.)

## Top-Level Structure

```
backend/
├── __init__.py
├── server_fastapi.py      # FastAPI app entry (mounts all API under src)
└── src/                   # Business logic by layer and domain
    ├── api/               # HTTP API routes (protocol and input parsing only)
    │   ├── engine.py      # Gateway / market / strategy / holdings / logs
    │   ├── backtest.py    # Backtest endpoints (files, strategy list, run/cancel)
    │   ├── system.py      # System status + DB views (orders/trades, contracts)
    │   └── schemas/       # API-level request / response models
    │       ├── common.py  # Shared response models (BaseResponse, ErrorResponse, etc., optional)
    │       └── engine.py  # Engine request bodies (AddStrategyRequest, etc.)
    ├── services/          # Domain service layer (business rules, response shaping)
    │   ├── main.py        # Gateway / market / orders-trades / logs
    │   ├── strategy.py    # Strategy lifecycle and metadata
    │   ├── backtest.py    # Backtest param validation + result shaping
    │   └── system.py      # System view wrapper (calls infra.db)
    ├── infra/             # Infrastructure (external system integration)
    │   ├── remote_client.py      # gRPC EngineClient (C++ live engine)
    │   ├── backtest_runner.py    # Invokes C++ backtest executable entry_backtest
    │   ├── db.py                 # PostgreSQL read-only queries (orders/trades, contracts)
    │   ├── state.py              # Global runtime state (live / backtest / websockets)
    │   ├── live_client_lifespan.py  # Live gRPC client and log thread lifecycle
    │   └── websockets.py         # Log and strategy WebSocket handlers
    ├── proto/             # gRPC stubs generated from .proto (EngineService)
    └── utils/             # Utilities (file listing, chart rendering, JSON, etc.)
```

### Call Flow: Live Engine

1. HTTP request hits the route in `src/api/engine.py` (e.g. `/api/gateway/connect`).
2. Route calls `MainService` / `StrategyService`:
   - Uses `_get_client_or_400()` to check `AppState.live.live_client`;
   - Handles parsing, response shaping, and error semantics (e.g. gateway/market status).
3. Service calls `infra.remote_client.EngineClient`:
   - Calls C++ EngineService via `src.proto.otrader_engine_pb2(_grpc)`;
   - Converts proto objects to frontend-friendly dicts (field names compatible with legacy OTrader Python engine).

### Call Flow: Backtest

1. HTTP request hits `src/api/backtest.py` (e.g. `/api/run_backtest`).
2. Route passes body to `BacktestService.run_backtest`:
   - Validates params and applies defaults (date range, fee, slippage, IV mode, etc.);
   - Merges `progress_info` and other extras into the result.
3. `BacktestService` calls `infra.backtest_runner.run_backtest_cpp`:
   - Collects parquet paths under `data/{symbol}/` by symbol + date range;
   - Builds CLI args and starts `entry_backtest` via `subprocess.Popen`;
   - Parses stdout/stderr for JSON result and progress events, returns a Python dict.
4. Cancel via `BacktestService.cancel_backtest` → `infra.backtest_runner.cancel_current_backtest`, using `AppState.backtest.backtest_proc` for the current C++ backtest process.

### Call Flow: System / DB Views

1. `src/api/system.py` exposes system status and read-only DB endpoints:
   - `/api/system/status`: backend + live engine status (`AppState.live.live_status`);
   - `/api/orders-trades/db` and `/api/database/contracts`: DB views.
2. DB routes call `services.system`:
   - Validates query params, normalises fields (e.g. `record_type`);
   - Shapes raw query results into the JSON structure expected by the frontend.
3. `services.system` calls `infra.db`:
   - `fetch_orders_trades_raw`: psycopg2 + SQL over orders/trades with UNION;
   - `fetch_contracts_summary`: equity/option summary view.

### State and WebSockets

- `infra.state.AppState` holds three runtime areas:
  - `AppState.live`: live gRPC client, live_status, log buffer, log thread state;
  - `AppState.backtest`: current backtest process handle;
  - `AppState.ws`: log and strategy WebSocket client sets.
- `infra.live_client_lifespan` (FastAPI lifespan):
  - Initialises `EngineClient`;
  - Starts live_status heartbeat task;
  - Starts log consumer thread (feeds `AppState.live.log_queue` to WS).
- `infra.websockets`:
  - Log WebSocket: consumes `log_queue` and pushes to all log_clients;
  - Strategy WebSocket: connection lifecycle (reserved for future server-side push).

## Main APIs Exposed to the Frontend

### Engine / Strategy / Holdings

- **Gateway / Market**
  - `POST /api/gateway/connect`: Connect IB gateway.
  - `POST /api/gateway/disconnect`: Disconnect IB gateway (also attempts to stop market data).
  - `GET  /api/gateway/status`: `{status, connected, detail}` gateway summary.
  - `GET  /api/market/status`: `{status, connected, detail}` market summary.
  - `POST /api/market/start`: Start market data.
  - `POST /api/market/stop`: Stop market data.

- **Strategy Metadata and Lifecycle**
  - `GET  /api/strategies`: List of running strategies.
  - `GET  /api/strategies/meta/strategy-classes`: Available strategy class names.
  - `GET  /api/strategies/meta/settings?class=XXX`: Default config for a strategy class.
  - `GET  /api/strategies/meta/portfolios`: Available portfolio names.
  - `GET  /api/strategies/meta/removed-strategies`: Removed strategy names.
  - `POST /api/strategies`: Create strategy; body `AddStrategyRequest { strategy_class, portfolio_name, setting }`.
  - `POST /api/strategies/restore`: Not supported in C++ mode; returns error.
  - `POST /api/strategies/{strategy_name}/init`: Init strategy.
  - `POST /api/strategies/{strategy_name}/start`: Start strategy.
  - `POST /api/strategies/{strategy_name}/stop`: Stop strategy.
  - `DELETE /api/strategies/{strategy_name}/remove`: Remove from engine (soft delete).
  - `DELETE /api/strategies/{strategy_name}/delete`: Hard delete strategy.

- **Holdings / Orders / Trades / Portfolios**
  - `GET /api/strategies/holdings`: Holdings view by strategy.
  - `GET /api/orders-trades`: Unified orders/trades from gRPC.
  - `GET /api/data/portfolios`: Portfolio names (for UI dropdown).

- **Logs**
  - `GET  /api/logs`: Recent logs (initial page load).
  - `GET  /logs`: Log list with configurable limit.
  - `POST /api/logs/clear`: Clear backend log buffer.
  - `WS   /ws/logs`: Real-time log subscription.
  - `WS   /ws/strategies`: Reserved strategy push channel (connection only for now).

### Backtest

- **Metadata and Files**
  - `GET /api/files`: List of `.dbn` / `.parquet` files.
  - `GET /api/backtest/strategies`: Strategy classes available for backtest (from C++ config).
  - `GET /api/file_info?path=...`: Single parquet time range, row count, etc.
  - `GET /api/backtest_duration`: Per-symbol covered date range summary.

- **Run / Cancel**
  - `POST /api/run_backtest`:
    - Body `Dict[str, Any]`: `parquet`, `strategy`, `strategy_setting`, `iv_price_mode`, `start_date`, `end_date`, fees, slippage, etc.
    - Response shape (legacy convention):
      - `status: "ok" | "error" | "cancelled"`
      - `result`: final metrics and curve data
      - `progress_info`: optional progress events (for frontend progress bar).
  - `POST /api/backtest/cancel`: Cancel current backtest process; returns `{status, message}`.

### System / DB Views

- `GET /api/system/status`: Health:
  - `backend.status`: `"running"` when this endpoint returns 200.
  - `live`: live engine status from `AppState.live.live_status`.
- `POST /api/system/restart_live`: Logical restart of live engine (Disconnect/Connect + Stop/Start MD).
- `GET /api/orders-trades/db`: Historical orders/trades from PostgreSQL (DB view).
- `GET /api/database/contracts`: Contract view (equity/option totals, per-chain summary, etc.).

## Interaction with the C++ Engine

### 1. gRPC Live Engine (Live Trading)

- C++ exposes `EngineService` (see `Otrader/proto/otrader_engine.proto`).
- Python uses stubs from `src/proto/otrader_engine_pb2(_grpc)`.
- `src/infra/remote_client.EngineClient` wraps all gRPC calls:
  - Connection and status: `ConnectGateway`, `DisconnectGateway`, `GetStatus`, `StartMarketData`, `StopMarketData`.
  - Strategy: `ListStrategies`, `AddStrategy`, `InitStrategy`, `StartStrategy`, `StopStrategy`, `RemoveStrategy`, `DeleteStrategy`.
  - Data: `GetOrdersAndTrades`, `GetStrategyHoldings`, `ListStrategyClasses`, `GetStrategyClassDefaults`, `GetPortfoliosMeta`, `GetRemovedStrategies`.
  - Log stream: `StreamLogs`, consumed by `live_client_lifespan` and pushed to `AppState.live.log_queue`.

The frontend never talks to gRPC directly; it uses HTTP API and WebSockets only.

### 2. C++ Backtest Executable `entry_backtest`

- Executable path: `Otrader/build/entry_backtest` (built by top-level `./build.sh`).
- `src/infra/backtest_runner.run_backtest_cpp`:
  - Resolves symbol + date range to a set of parquet paths;
  - Builds CLI args and starts `entry_backtest` via `subprocess.Popen`;
  - Parses stdout/stderr into final JSON payload and progress events, returns a Python dict.
- `src/infra/backtest_runner.cancel_current_backtest`:
  - Uses `AppState.backtest.backtest_proc` for the current backtest process and provides a single cancel path.

The frontend drives this via `/api/run_backtest` and `/api/backtest/cancel`.
