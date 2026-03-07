# Frontend Architecture Overview

Next.js frontend (App Router). Responsibilities:

- Live trading state display (gateway, market, strategies, holdings, logs)
- Backtest configuration, submission, and result display
- REST and WebSocket communication with the backend

## Top-Level Structure

```
frontend/
├── app/                     # App Router entry and pages
│   ├── layout.tsx           # Global layout (nav, status bar, theme)
│   ├── page.tsx             # Home (overview / entry)
│   ├── engine/              # Live engine pages (gateway, strategy, holdings)
│   ├── backtest/            # Backtest config and result pages
│   └── system/              # System status and DB view pages
├── components/              # Reusable UI components
│   ├── layout/              # Layout (Shell, Sidebar, Header, StatusBar)
│   ├── engine/              # Engine components (strategy table, holdings table, log panel)
│   ├── backtest/            # Backtest form, progress bar, result view
│   └── common/              # Shared (tables, dialogs, buttons, chart containers)
├── lib/                     # Non-UI logic (API calls, data models, hooks)
│   ├── api.ts               # HTTP calls to backend
│   ├── ws.ts                # WebSocket (logs / strategies)
│   ├── types.ts             # TypeScript types (aligned with backend JSON)
│   └── format.ts            # Display formatting (time, currency, percent)
├── styles/                  # Global styles and design system (Tailwind / CSS modules)
└── public/                  # Static assets
```

## Backend Interaction

### Live Engine

- **Health and status**
  - `GET /api/system/status` returns `{ backend, live }` with `status`, `connected`, `detail`.
  - The global StatusBar polls this endpoint and displays Backend / Live Engine status (green / yellow / red).

- **Gateway and market**
  - Endpoints: `POST /api/gateway/connect`, `disconnect`; `GET /api/gateway/status`; `GET /api/market/status`; `POST /api/market/start`, `stop`.
  - The Engine page controls and displays IB gateway and market data status via these endpoints.

- **Strategies and holdings**
  - Strategy list: `GET /api/strategies`
  - Strategy classes and defaults: `GET /api/strategies/meta/strategy-classes`, `.../settings`
  - Lifecycle: `POST /api/strategies`, `/{name}/init`, `/{name}/start`, `/{name}/stop`, `/{name}/remove`, `/{name}/delete`
  - Holdings: `GET /api/strategies/holdings`
  - Updates: `GET /api/strategies/updates`, `.../clear`
  - The strategy page creates, starts, and stops strategies and displays holdings.

- **Orders, trades, portfolios**
  - `GET /api/orders-trades`: Order/Trade list (backend gRPC wrapper).
  - `GET /api/data/portfolios`: Portfolio names.

- **Logs**
  - History: `GET /logs`, `GET /api/logs`
  - Live: WebSocket `/ws/logs`; `lib/ws.ts` establishes the connection; new lines are appended to the log view.
  - Clear: `POST /api/logs/clear`

### Backtest

- **Files and strategy list**
  - `GET /api/files`: `.dbn` / `.parquet` file list.
  - `GET /api/backtest/strategies`: C++ backtest strategy class names (dropdown).
  - `GET /api/file_info`: Single parquet time range and meta.
  - `GET /api/backtest_duration`: Covered-period summary.

- **Run and cancel**
  - `POST /api/run_backtest`: Request body includes parquet/symbol, date range, params. Response: `status`, `result`, optional `progress_info`.
  - `POST /api/backtest/cancel`: Cancels the current run.
  - The progress bar and result charts consume `progress_info` and `result`.

## State and UI Layers

- **Page-level** (`app/*`): Layout (nav, status bar, main content); data fetching and view selection.
- **Domain components** (`components/engine/*`, `components/backtest/*`): Receive data and callbacks via props; local interaction and display (tables, charts, forms).
- **`lib/api.ts` / `lib/ws.ts`**: Encapsulate backend URLs; expose `fetchEngineStatus()`, `connectGateway()`, `subscribeLogs()`, etc. Pages and components call these helpers instead of backend endpoints directly.
- **Decoupling**: Backend internals can change without UI changes as long as the HTTP and WebSocket contracts remain unchanged.
