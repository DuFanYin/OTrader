#!/bin/bash
# Unified system launcher (refactor §9 部署与启动顺序):
# 1. Gateway (entry_gateway) - ZMQ REP/PUB for Order/Trade
# 2. Market Data (entry_market_data) - ZMQ REP/PUB for Snapshot
# 3. Runtime (entry_live_grpc) - connects to Gateway/Market Data via ZMQ, exposes gRPC
# 4. Backend (Python FastAPI)
# 5. Frontend (Next.js)
#
# Env (from .env or defaults): GATEWAY_REP_ADDR, GATEWAY_PUB_ADDR, MARKETDATA_REP_ADDR,
#   MARKETDATA_PUB_ADDR, DATABASE_URL, TRADIER_TOKEN, IB_HOST, IB_PORT, IB_CLIENT_ID, IB_ACCOUNT
# Usage: ./system_up.sh [build|run|dev|clean]

set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
[[ -f .env ]] && set -a && source .env && set +a
MODE=${1:-dev}

# ZMQ addresses (defaults per refactor doc §2)
export GATEWAY_REP_ADDR="${GATEWAY_REP_ADDR:-tcp://127.0.0.1:5555}"
export GATEWAY_PUB_ADDR="${GATEWAY_PUB_ADDR:-tcp://127.0.0.1:5556}"
export MARKETDATA_REP_ADDR="${MARKETDATA_REP_ADDR:-tcp://127.0.0.1:5557}"
export MARKETDATA_PUB_ADDR="${MARKETDATA_PUB_ADDR:-tcp://127.0.0.1:5558}"

GATEWAY_PID=""
MARKETDATA_PID=""
LIVE_PID=""
BACKEND_PID=""
FRONTEND_PID=""

cleanup() {
  echo ""
  echo "Shutting down..."
  [ -n "$FRONTEND_PID" ] && echo "Stopping frontend (PID: $FRONTEND_PID)..." && kill "$FRONTEND_PID" 2>/dev/null || true
  [ -n "$BACKEND_PID" ] && echo "Stopping backend (PID: $BACKEND_PID)..." && kill "$BACKEND_PID" 2>/dev/null || true
  [ -n "$LIVE_PID" ] && echo "Stopping Runtime (PID: $LIVE_PID)..." && kill "$LIVE_PID" 2>/dev/null || true
  [ -n "$MARKETDATA_PID" ] && echo "Stopping Market Data (PID: $MARKETDATA_PID)..." && kill "$MARKETDATA_PID" 2>/dev/null || true
  [ -n "$GATEWAY_PID" ] && echo "Stopping Gateway (PID: $GATEWAY_PID)..." && kill "$GATEWAY_PID" 2>/dev/null || true
  exit 0
}

trap cleanup SIGINT SIGTERM

build() {
  if [ ! -d "$ROOT/app/frontend/node_modules" ]; then
    echo "Frontend dependencies not found. Installing..."
    cd "$ROOT/app/frontend"
    npm install
    cd "$ROOT"
  fi
  echo "Build complete! Run ./build.sh to build C++ targets (entry_backtest, entry_system)."
}

run() {
  echo "Starting Python backend server only..."
  cd "$ROOT/app/backend"
  exec python3 -m server_fastapi
}

wait_for_backend() {
  echo "Waiting for backend to be ready..."
  for i in {1..30}; do
    if [ -n "$BACKEND_PID" ] && ! kill -0 "$BACKEND_PID" 2>/dev/null; then
      echo "Error: backend process (PID: $BACKEND_PID) exited unexpectedly."
      [ -f /tmp/backend.log ] && tail -n 40 /tmp/backend.log
      return 1
    fi
    if curl -s http://localhost:8080/api/files > /dev/null 2>&1; then
      echo "Backend is ready!"
      return 0
    fi
    sleep 1
  done
  echo "Warning: Backend did not become ready in time"
  [ -f /tmp/backend.log ] && tail -n 40 /tmp/backend.log
  return 1
}

dev() {
  BUILD_DIR="$ROOT/Otrader/build"
  IBJTS_LIB_DIR="$ROOT/Otrader/thirdparty/IBJts/build-apple/lib"
  [ -d "$IBJTS_LIB_DIR" ] && export DYLD_LIBRARY_PATH="$IBJTS_LIB_DIR:${DYLD_LIBRARY_PATH:-}"

  # 1) Gateway
  GATEWAY_BIN="$BUILD_DIR/entry_gateway"
  if [ ! -x "$GATEWAY_BIN" ]; then
    echo "Error: entry_gateway not found. Build with: ./build.sh"
    exit 1
  fi
  echo "Starting Gateway (entry_gateway)..."
  "$GATEWAY_BIN" > /tmp/gateway.log 2>&1 &
  GATEWAY_PID=$!
  echo "  Gateway started (PID: $GATEWAY_PID, REP: $GATEWAY_REP_ADDR, PUB: $GATEWAY_PUB_ADDR)"
  sleep 1

  # 2) Market Data (requires DATABASE_URL, TRADIER_TOKEN)
  MARKETDATA_BIN="$BUILD_DIR/entry_market_data"
  if [ ! -x "$MARKETDATA_BIN" ]; then
    echo "Error: entry_market_data not found. Build with: ./build.sh"
    kill "$GATEWAY_PID" 2>/dev/null || true
    exit 1
  fi
  if [ -z "$DATABASE_URL" ] || [ -z "$TRADIER_TOKEN" ]; then
    echo "Warning: DATABASE_URL or TRADIER_TOKEN not set. Market Data may fail to connect."
  fi
  echo "Starting Market Data (entry_market_data)..."
  "$MARKETDATA_BIN" > /tmp/marketdata.log 2>&1 &
  MARKETDATA_PID=$!
  echo "  Market Data started (PID: $MARKETDATA_PID, REP: $MARKETDATA_REP_ADDR, PUB: $MARKETDATA_PUB_ADDR)"
  sleep 1

  # 3) Runtime (entry_live_grpc)
  LIVE_BIN="$BUILD_DIR/entry_live_grpc"
  if [ ! -x "$LIVE_BIN" ]; then
    echo "Error: entry_live_grpc not found. Build with: ./build.sh"
    kill "$MARKETDATA_PID" "$GATEWAY_PID" 2>/dev/null || true
    exit 1
  fi
  echo "Starting Runtime (entry_live_grpc)..."
  "$LIVE_BIN" > /tmp/live_grpc.log 2>&1 &
  LIVE_PID=$!
  echo "  Runtime started (PID: $LIVE_PID)"

  # 4) Frontend deps
  if [ ! -d "$ROOT/app/frontend/node_modules" ]; then
    echo "Installing frontend dependencies..."
    (cd "$ROOT/app/frontend" && npm install)
  fi

  # 5) Backend
  echo "Starting backend..."
  (cd "$ROOT/app/backend" && python -m server_fastapi > /tmp/backend.log 2>&1) &
  BACKEND_PID=$!
  wait_for_backend

  # 6) Frontend
  echo "Starting frontend..."
  (cd "$ROOT/app/frontend" && npm run dev > /tmp/frontend.log 2>&1) &
  FRONTEND_PID=$!

  echo ""
  echo "=========================================="
  echo "  Gateway:     /tmp/gateway.log"
  echo "  Market Data: /tmp/marketdata.log"
  echo "  Runtime:     /tmp/live_grpc.log"
  echo "  Backend:     http://localhost:8080"
  echo "  Frontend:    http://localhost:3000"
  echo "=========================================="
  echo "  Logs: tail -f /tmp/gateway.log /tmp/marketdata.log /tmp/live_grpc.log"
  echo "  Press Ctrl+C to stop all"
  echo ""
  wait
}

case "$MODE" in
  build) build ;;
  run)   run "$2" "$3" ;;
  dev)   dev ;;
  clean) rm -rf app/frontend/node_modules app/frontend/.next ;;
  *)     echo "Usage: $0 [build|run|dev|clean]"; exit 1 ;;
esac
