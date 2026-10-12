#!/usr/bin/env bash
# The C++ engine build — thin wrapper over CMake presets (see engine/CMakePresets.json).
# Usage: ./build.sh [d|r] [0] [s] [g]
#   d = Debug, r = Release (default Release)
#   0 = wipe build dir and reconfigure
#   s = strategy-only build (strategies + entry_backtest)
#   g = include the IB gateway (BUILD_GATEWAY=ON; the first one fetches the TWS API, scripts/fetch_ibjts.sh)
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_DIR="$REPO_ROOT/engine"

BUILD_TYPE=Release
DO_CLEAN=0
STRATEGY_ONLY=0
WITH_GATEWAY=0
for a in "$@"; do
  case "$a" in
    d) BUILD_TYPE=Debug ;;
    r) BUILD_TYPE=Release ;;
    0) DO_CLEAN=1 ;;
    s) STRATEGY_ONLY=1 ;;
    g) WITH_GATEWAY=1 ;;
  esac
done

# Pick the preset for the options. Optra builds on Linux only.
if [[ "$(uname -s)" != Linux ]]; then
  echo "Unsupported host: $(uname -s) (Optra builds on Linux)"; exit 1
fi
if [[ "$WITH_GATEWAY" -eq 1 ]]; then PRESET=linux-gateway
elif [[ "$BUILD_TYPE" == Debug ]]; then PRESET=linux-debug
else PRESET=linux; fi

# LetsBeRational (implied vol) is not in git; fetch it once.
if [[ ! -f "$SRC_DIR/thirdparty/lets_be_rational/src/lets_be_rational.cpp" ]]; then
  "$REPO_ROOT/scripts/fetch_lets_be_rational.sh"
fi

# Nor are the TWS API and the Intel decimal library the IB gateway compiles in; fetch them once.
if [[ "$WITH_GATEWAY" -eq 1 && ( ! -f "$SRC_DIR/thirdparty/IBJts/source/cppclient/client/EClientSocket.h" \
      || ! -f "$SRC_DIR/thirdparty/IntelRDFP/libbid.a" ) ]]; then
  "$REPO_ROOT/scripts/fetch_ibjts.sh"
fi

[[ "$DO_CLEAN" -eq 1 ]] && rm -rf "$SRC_DIR/build"

# cmake --preset resolves CMakePresets.json relative to CWD → run from the source dir.
cd "$SRC_DIR"
cmake --preset "$PRESET"
if [[ "$STRATEGY_ONLY" -eq 1 ]]; then
  cmake --build build --target strategies --target entry_backtest -j"$(getconf _NPROCESSORS_ONLN)"
else
  cmake --build --preset "$PRESET" -j"$(getconf _NPROCESSORS_ONLN)"
fi

echo ""
echo "========================================"
echo "engine build done ($PRESET, $BUILD_TYPE): $SRC_DIR/build"
echo "  entry_backtest = $SRC_DIR/build/entry_backtest"
echo "  entry_live     = $SRC_DIR/build/entry_live  (live runtime: gRPC + in-process market-data + gateway)"
echo "  entry_simulate = $SRC_DIR/build/entry_simulate  (the live runtime on a simulated market)"
echo "  analytics      = $SRC_DIR/build/quant/analytics/libanalytics_c.so  (Optra's server loads it)"
echo "========================================"
