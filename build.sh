#!/usr/bin/env bash
# Otrader C++ build — thin wrapper over CMake presets (see Otrader/CMakePresets.json).
# Usage: ./build.sh [d|r] [0] [s] [g]
#   d = Debug, r = Release (default Release)
#   0 = wipe build dir and reconfigure
#   s = strategy-only build (strategies + entry_backtest)
#   g = include the IB gateway (BUILD_GATEWAY=ON; requires Otrader/thirdparty/IBJts)
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_DIR="$REPO_ROOT/Otrader"

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

# Pick the preset for this host + options.
case "$(uname -s)" in
  Darwin)
    if [[ "$WITH_GATEWAY" -eq 1 ]]; then PRESET=macos-gateway
    elif [[ "$BUILD_TYPE" == Debug ]]; then PRESET=macos-debug
    else PRESET=macos; fi ;;
  Linux) PRESET=linux ;;
  *) echo "Unsupported host: $(uname -s)"; exit 1 ;;
esac

# IB gateway needs the vendored twsapi dylib built first.
if [[ "$WITH_GATEWAY" -eq 1 ]]; then
  IBJTS_DIR="$SRC_DIR/thirdparty/IBJts"
  if [[ ! -d "$IBJTS_DIR" ]]; then
    echo "Error: gateway build needs IBJts at $IBJTS_DIR (extract twsapi_macunix.*.zip there)."
    exit 1
  fi
  if [[ ! -f "$IBJTS_DIR/build-apple/lib/"libtwsapi.* ]]; then
    echo ">>> Building IBJts twsapi..."
    ( cd "$IBJTS_DIR" \
      && protoc -I source/proto --cpp_out=source/cppclient/client/protobufUnix source/proto/*.proto \
      && cmake -S . -B build-apple -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/opt/homebrew -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
      && cmake --build build-apple -j"$(getconf _NPROCESSORS_ONLN)" )
  fi
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
echo "Otrader build done ($PRESET, $BUILD_TYPE): $SRC_DIR/build"
echo "  entry_backtest = $SRC_DIR/build/entry_backtest"
echo "  entry_system   = $SRC_DIR/build/entry_system  (--mode=gateway|market|live|all)"
echo "========================================"
