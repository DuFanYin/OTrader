# OTrader Backend

Backend API server for OTrader built with FastAPI.

## File Structure

```
backend/
├── requirements.txt             # Python dependencies
├── README.md                    # Documentation
└── src/                         # Source code
    ├── __init__.py
    ├── main.py                  # FastAPI application entry point
    ├── api/                     # API route modules
    │   ├── engine_api.py        # Unified engine API endpoints
    │   └── static_api.py        # Static file serving endpoints
    ├── infrastructure/          # Infrastructure layer
    │   ├── state.py             # Global application state management
    │   ├── events.py            # Engine event listeners and loguru integration
    │   └── websockets.py        # WebSocket handlers for real-time communication
    └── services/                # Service layer (business logic and models)
        ├── models.py            # Pydantic models for API requests/responses
        ├── service_main.py      # Main engine service (gateway, database, portfolio, logs)
        └── service_strategy.py  # Strategy management service
```

## File Descriptions

### Root Files
- **requirements.txt** - Python package dependencies
- **README.md** - This documentation

### Source Files (`src/`)

#### Main Application
- **main.py** - FastAPI application entry point, middleware setup, WebSocket routes, static file serving

#### API Layer (`api/`)
- **engine_api.py** - Unified engine API endpoints (gateway, strategies, data, logs)
- **static_api.py** - Static file serving endpoints (HTML pages, frontend assets)

#### Infrastructure Layer (`infrastructure/`)
- **state.py** - Global application state management (engine, logs, WebSocket clients)
- **events.py** - Engine event listeners and loguru integration setup
- **websockets.py** - WebSocket handlers for real-time communication (logs, strategies)

#### Service Layer (`services/`)
- **models.py** - Pydantic models for API requests and responses
- **service_main.py** - Main engine service (gateway management, database operations, portfolio data, log management)
- **service_strategy.py** - Strategy management service (CRUD operations, lifecycle management, holdings)

## Architecture Overview

**Layered Design:**
- **API Layer** (`api/`) - HTTP routes and request handling
- **Service Layer** (`services/`) - Business logic, models, and orchestration
- **Infrastructure Layer** (`infrastructure/`) - External integrations and state management

**Key Features:**
- FastAPI framework with automatic API documentation
- WebSocket support for real-time communication
- Loguru integration for real-time log streaming
- Event-driven architecture with engine integration
- Type safety with mypy and ruff linting
- CORS support for frontend integration

## API Endpoints

### Gateway Management (`/api/gateway/`)
- `POST /connect` - Connect to trading gateway
- `POST /disconnect` - Disconnect from trading gateway
- `GET /status` - Get gateway connection status

### Strategy Management (`/api/strategies/`)
- `GET /` - List all strategies
- `GET /updates` - Get strategy updates (polling)
- `GET /meta/strategy-classes` - Get available strategy classes
- `GET /meta/portfolios` - Get available portfolios
- `GET /meta/removed-strategies` - Get removed strategies
- `GET /holdings` - Get strategy holdings data
- `POST /` - Add new strategy
- `POST /restore` - Restore strategy
- `POST /{name}/init` - Initialize strategy
- `POST /{name}/start` - Start strategy
- `POST /{name}/stop` - Stop strategy
- `DELETE /{name}/remove` - Remove strategy
- `DELETE /{name}/delete` - Delete strategy

### Data Management (`/api/`)
- `GET /orders-trades` - Get combined orders and trades data

### Market Data (`/api/data/`)
- `GET /portfolios` - Get portfolio names

### Log Management (`/api/logs/`)
- `GET /` - Get recent logs (JSON format)

### Frontend Serving
- `GET /` - Redirect to Strategy Manager
- `GET /strategy/strategy-manager.html` - Serve Strategy Manager page
- `GET /strategy/strategy-holding.html` - Serve Strategy Holdings page
- `GET /admin/orders-trades.html` - Serve Orders & Trades page

### WebSocket Endpoints
- `WS /ws/logs` - Real-time log streaming
- `WS /ws/strategies` - Real-time strategy updates