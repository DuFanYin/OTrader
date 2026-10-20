#!/bin/bash
# The whole stack for development, stopped together with Ctrl+C: an engine, the server (FastAPI, on
# $PORT, 8080 by default), a research worker (it runs the backtests and experiments the server
# queues) and the page (Vite dev server, :5173).
#
#   ./scripts/dev.sh sim   the engine on a simulated market (engine/build/entry_simulate): synthetic SPY
#                          days are written to data/backtest/SPY when there are none (the three months before
#                          today, for backtests and experiments, and today's, which the engine replays
#                          at the wall clock's pace), orders fill at the simulated quotes, and the
#                          account is kept in data/sim/. Nothing to connect to, no account needed.
#   ./scripts/dev.sh       the live engine (engine/build/entry_live; gRPC, market data and the gateway
#                          in one process) on Interactive Brokers: IB Gateway or TWS running, and .env
#                          with IB_ACCOUNT (a paper account) and, if not the defaults, OPTRA_DB, IB_HOST,
#                          IB_PORT, IB_CLIENT_ID, IB_MARKET_DATA_LINES, IB_MARKET_DATA_TYPE.
#
# LIVE_GRPC_TARGET is where the engine listens and the server asks (default localhost:50051); PORT is
# the server's (default 8080), which the page proxies to. The page adds the person's key
# ($OPTRA_HOME/access.key, default ~/.optra) to what it sends, as Optra's front door does once signed in:
# on this machine its user is the person (app/server/optra/infra/access.py).

set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
[[ -f .env ]] && set -a && source .env && set +a
PORT="${PORT:-8080}"
WEB_HOST="${WEB_HOST:-localhost}"  # where the page listens: 0.0.0.0 in a container (compose.yaml)
MODE="${1:-live}"
export LIVE_GRPC_TARGET="${LIVE_GRPC_TARGET:-localhost:50051}"
UV="$(command -v uv || echo "$HOME/.local/bin/uv")"

LIVE_PID=""
BACKEND_PID=""
WORKER_PID=""
FRONTEND_PID=""

cleanup() {
  echo ""
  echo "Shutting down..."
  local pids=() p _
  for p in "$FRONTEND_PID" "$WORKER_PID" "$BACKEND_PID" "$LIVE_PID"; do [ -n "$p" ] && pids+=("$p"); done
  kill "${pids[@]}" 2>/dev/null || true
  # A worker waiting on the server's long poll, or a server on its open connections, can take a while.
  for _ in $(seq 10); do
    local alive=()
    for p in "${pids[@]}"; do kill -0 "$p" 2>/dev/null && alive+=("$p"); done
    pids=("${alive[@]}")
    [ ${#pids[@]} -eq 0 ] && break
    sleep 0.5
  done
  [ ${#pids[@]} -gt 0 ] && kill -9 "${pids[@]}" 2>/dev/null || true
  exit 0
}
trap cleanup SIGINT SIGTERM

# Synthetic SPY days from FIRST to LAST that data/backtest/SPY lacks (the generator refuses a day already there),
# priced rich (implied 18% against a realised 13%: the premium a seller earns).
synthetic_days() {
  (cd "$ROOT/app/server" && "$UV" run -q python -m optra.data.synthetic_days --start "$1" --end "$2" \
    --out ../../data/backtest --symbol SPY --seed "${3:-1}" --implied-vol 0.18 --realized-vol 0.13 > /dev/null)
}

if [ "$MODE" = sim ]; then
  SPY="$ROOT/data/backtest/SPY"
  if ls "$SPY"/SPY-*/*.parquet > /dev/null 2>&1 && ! ls "$SPY"/synthetic-*.json > /dev/null 2>&1; then
    echo "Error: data/backtest/SPY holds days that are not synthetic; sim writes synthetic days there. Use a clone of its own."
    exit 1
  fi
  # The three months before today, from the day after the last one there.
  start=$(date -d '3 months ago' +%F)
  last=$(ls "$SPY"/SPY-*/*.parquet 2>/dev/null | xargs -rn1 basename | sort | tail -1 | cut -c1-8)
  [ -n "$last" ] && [ "$(date -d "$last + 1 day" +%F)" \> "$start" ] && start=$(date -d "$last + 1 day" +%F)
  if [ ! "$start" \> "$(date -d yesterday +%F)" ]; then
    echo "Writing synthetic SPY days $start to $(date -d yesterday +%F)..."
    synthetic_days "$start" "$(date -d yesterday +%F)"
  fi
  # Today's, or the next weekday's: the engine replays it from its open, now.
  day=$(date +%F)
  while [ "$(date -d "$day" +%u)" -gt 5 ]; do day=$(date -d "$day + 1 day" +%F); done
  file="$SPY/SPY-${day:0:7}/${day//-/}.parquet"
  [ -f "$file" ] || synthetic_days "$day" "$day" "${day//-/}"
  mkdir -p "$ROOT/data/sim"
  export OPTRA_DB="$ROOT/data/sim/optra.db"
  ENGINE_BIN="$ROOT/engine/build/entry_simulate"
  # Beside the demo strategies, those installed for the paper account (Home's own strategy: Install for paper).
  mkdir -p "$ROOT/engine/build/strategies/paper"
  ENGINE_ARGS=(--listen "$LIVE_GRPC_TARGET" --resume --strategies "$ROOT/engine/build/strategies/paper" "$file")
else
  ENGINE_BIN="$ROOT/engine/build/entry_live"
  ENGINE_ARGS=()
fi
if [ ! -x "$ENGINE_BIN" ]; then
  echo "Error: $(basename "$ENGINE_BIN") not found. Build with: ./build.sh r"
  exit 1
fi
echo "Starting the engine ($(basename "$ENGINE_BIN"))..."
"$ENGINE_BIN" "${ENGINE_ARGS[@]}" > /tmp/optra-engine.log 2>&1 &
LIVE_PID=$!

[ -d "$ROOT/app/web/node_modules" ] || (cd "$ROOT/app/web" && npm ci)

# The server's environment, then its programs straight from it: a signal reaches them, not a uv in between.
(cd "$ROOT/app/server" && "$UV" sync -q)
VENV="$ROOT/app/server/.venv/bin"
echo "Starting the server on :$PORT..."
(cd "$ROOT/app/server" && exec "$VENV/uvicorn" main:app --host 127.0.0.1 --port "$PORT" --timeout-graceful-shutdown 5 > /tmp/optra-server.log 2>&1) &
BACKEND_PID=$!
for _ in $(seq 30); do
  if ! kill -0 "$BACKEND_PID" 2>/dev/null; then
    echo "Error: the server exited:"; tail -n 40 /tmp/optra-server.log; cleanup
  fi
  curl -s "http://127.0.0.1:$PORT/api/files" > /dev/null 2>&1 && break
  sleep 1
done

echo "Starting a research worker..."
(cd "$ROOT/app/server" && exec "$VENV/python" -m optra.research.worker --server "http://127.0.0.1:$PORT" \
  > /tmp/optra-worker.log 2>&1) &
WORKER_PID=$!

echo "Starting the page..."
(cd "$ROOT/app/web" && OPTRA_SERVER="http://127.0.0.1:$PORT" \
  OPTRA_KEY_FILE="${OPTRA_HOME:-$HOME/.optra}/access.key" exec npm run dev -- --host "$WEB_HOST" \
  > /tmp/optra-web.log 2>&1) &
FRONTEND_PID=$!

echo ""
echo "  Engine:   $(basename "$ENGINE_BIN") on $LIVE_GRPC_TARGET   (/tmp/optra-engine.log)"
echo "  Server:   http://localhost:$PORT   (/tmp/optra-server.log)"
echo "  Worker:   /tmp/optra-worker.log"
echo "  Page:     http://localhost:5173    (/tmp/optra-web.log)"
echo "  Ctrl+C stops them all"
wait
