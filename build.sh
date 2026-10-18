#!/usr/bin/env bash
# The C++ engine build — thin wrapper over CMake presets (see engine/CMakePresets.json).
# Usage: ./build.sh [d|r] [0] [s] [g] | ./build.sh p
#   d = Debug, r = Release (default Release)
#   0 = wipe build dir and reconfigure
#   s = the strategy libraries only (strategy/{demo,research,paper,live}/*.cpp into build/strategies: the
#       programs load them as they are, no rebuild)
#   g = include the IB gateway (BUILD_GATEWAY=ON; the first one fetches the TWS API, scripts/fetch.sh ibjts);
#       a Release build that has it keeps it without g, until 0
#   p = only the server's Python gRPC stubs (app/server/optra/proto) from engine/entry/optra_engine.proto, after
#       editing it; CMake generates the C++ side at build time
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_DIR="$REPO_ROOT/engine"

BUILD_TYPE=Release
DO_CLEAN=0
STRATEGY_ONLY=0
WITH_GATEWAY=0
PROTO_ONLY=0
for a in "$@"; do
  case "$a" in
    p) PROTO_ONLY=1 ;;
    d) BUILD_TYPE=Debug ;;
    r) BUILD_TYPE=Release ;;
    0) DO_CLEAN=1 ;;
    s) STRATEGY_ONLY=1 ;;
    g) WITH_GATEWAY=1 ;;
  esac
done

if [[ "$PROTO_ONLY" -eq 1 ]]; then
  # In the server's uv environment; grpcio-tools pinned to the locked grpcio, so the stubs never require a newer
  # grpcio than the one installed.
  OUT="$REPO_ROOT/app/server/optra/proto"
  (cd "$REPO_ROOT/app/server" && uv run --with "grpcio-tools==1.82.1" python -m grpc_tools.protoc \
      -I "$SRC_DIR/entry" --python_out="$OUT" --grpc_python_out="$OUT" "$SRC_DIR/entry/optra_engine.proto")
  # The generated stub imports its messages as a top-level module; the server imports optra.proto.
  sed -i 's/^import optra_engine_pb2/from optra.proto import optra_engine_pb2/' "$OUT/optra_engine_pb2_grpc.py"
  echo "build.sh: Python stubs in $OUT"
  exit 0
fi

# Pick the preset for the options. Optra builds on Linux only.
if [[ "$(uname -s)" != Linux ]]; then
  echo "Unsupported host: $(uname -s) (Optra builds on Linux)"; exit 1
fi
# A Release build that had the IB gateway keeps it: an entry_live without one cannot trade, and a
# live engine restarted runs the entry_live built last. 0 starts again from nothing.
if [[ "$WITH_GATEWAY" -eq 0 && "$DO_CLEAN" -eq 0 && "$BUILD_TYPE" == Release ]] &&
   grep -q '^BUILD_GATEWAY:BOOL=ON' "$SRC_DIR/build/CMakeCache.txt" 2>/dev/null; then
  echo "build.sh: engine/build has the IB gateway, so it keeps it (./build.sh r 0 builds without)"
  WITH_GATEWAY=1
fi
if [[ "$WITH_GATEWAY" -eq 1 ]]; then PRESET=linux-gateway
elif [[ "$BUILD_TYPE" == Debug ]]; then PRESET=linux-debug
else PRESET=linux; fi

# LetsBeRational (implied vol) is not in git; fetch it once.
if [[ ! -f "$SRC_DIR/thirdparty/lets_be_rational/src/lets_be_rational.cpp" ]]; then
  "$REPO_ROOT/scripts/fetch.sh" lets_be_rational
fi

# Nor are the TWS API and the Intel decimal library the IB gateway compiles in; fetch them once.
if [[ "$WITH_GATEWAY" -eq 1 && ( ! -f "$SRC_DIR/thirdparty/IBJts/source/cppclient/client/EClientSocket.h" \
      || ! -f "$SRC_DIR/thirdparty/IntelRDFP/libbid.a" ) ]]; then
  "$REPO_ROOT/scripts/fetch.sh" ibjts
fi

[[ "$DO_CLEAN" -eq 1 ]] && rm -rf "$SRC_DIR/build"

# cmake --preset resolves CMakePresets.json relative to CWD → run from the source dir.
cd "$SRC_DIR"
# One build at a time in build/: the server takes the same lock to build a strategy (research/strategies.py).
mkdir -p build
exec 8>build/.build.lock
flock -n 8 || { echo "waiting for another build in $SRC_DIR/build to finish..."; flock 8; }
cmake --preset "$PRESET"
if [[ "$STRATEGY_ONLY" -eq 1 ]]; then
  cmake --build build --target optra_strategies -j"$(getconf _NPROCESSORS_ONLN)"
else
  cmake --build --preset "$PRESET" -j"$(getconf _NPROCESSORS_ONLN)"
fi

echo ""
echo "========================================"
echo "engine build done ($PRESET, $BUILD_TYPE): $SRC_DIR/build"
echo "  entry_backtest = $SRC_DIR/build/entry_backtest"
echo "  entry_live     = $SRC_DIR/build/entry_live  (live runtime: gRPC + in-process market-data + gateway)"
echo "  entry_simulate = $SRC_DIR/build/entry_simulate  (the live runtime on a simulated market)"
echo "  strategies     = $SRC_DIR/build/strategies/  (a library per file of strategy/: demo, research/, paper/, live/)"
echo "  analytics      = $SRC_DIR/build/quant/analytics/libanalytics_c.so  (Optra's server loads it)"
echo "========================================"
