#!/bin/bash
# The whole stack for development, stopped together with Ctrl+C:
#   the live engine (engine/build/entry_live; gRPC, market data and the gateway in one process),
#   the server (FastAPI, on $PORT, 8080 by default), and the page (Vite dev server, :5173).
# Env (from .env or defaults): OPTRA_DB, IB_HOST, IB_PORT, IB_CLIENT_ID, IB_ACCOUNT,
#   IB_MARKET_DATA_LINES, IB_MARKET_DATA_TYPE. Where the deployed server holds 8080 (the ThinkPad),
#   give another PORT; `npm run dev` still proxies to 8080, so open the server's port instead.
# Usage: ./scripts/dev.sh

set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
[[ -f .env ]] && set -a && source .env && set +a
PORT="${PORT:-8080}"

LIVE_PID=""
BACKEND_PID=""
FRONTEND_PID=""

cleanup() {
  echo ""
  echo "Shutting down..."
  [ -n "$FRONTEND_PID" ] && kill "$FRONTEND_PID" 2>/dev/null || true
  [ -n "$BACKEND_PID" ] && kill "$BACKEND_PID" 2>/dev/null || true
  [ -n "$LIVE_PID" ] && kill "$LIVE_PID" 2>/dev/null || true
  exit 0
}
trap cleanup SIGINT SIGTERM

LIVE_BIN="$ROOT/engine/build/entry_live"
if [ ! -x "$LIVE_BIN" ]; then
  echo "Error: entry_live not found. Build with: ./build.sh"
  exit 1
fi
echo "Starting the engine (entry_live)..."
"$LIVE_BIN" > /tmp/live_grpc.log 2>&1 &
LIVE_PID=$!

[ -d "$ROOT/app/web/node_modules" ] || (cd "$ROOT/app/web" && npm ci)

echo "Starting the server on :$PORT..."
(cd "$ROOT/app/server" && uv run uvicorn main:app --host 127.0.0.1 --port "$PORT" > /tmp/optra-server.log 2>&1) &
BACKEND_PID=$!
for _ in $(seq 30); do
  if ! kill -0 "$BACKEND_PID" 2>/dev/null; then
    echo "Error: the server exited:"; tail -n 40 /tmp/optra-server.log; cleanup
  fi
  curl -s "http://127.0.0.1:$PORT/api/files" > /dev/null 2>&1 && break
  sleep 1
done

echo "Starting the page..."
(cd "$ROOT/app/web" && npm run dev > /tmp/optra-web.log 2>&1) &
FRONTEND_PID=$!

echo ""
echo "  Engine:   /tmp/live_grpc.log"
echo "  Server:   http://localhost:$PORT   (/tmp/optra-server.log)"
echo "  Page:     http://localhost:5173    (/tmp/optra-web.log)"
echo "  Ctrl+C stops all three"
wait
