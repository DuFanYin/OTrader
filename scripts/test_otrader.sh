#!/usr/bin/env bash
# Otrader 测试：回测(bt) / 实盘(live) / 仅构建(build)。与 build.sh、test_gtest.sh 共用 BUILD_DIR，只编本脚本要跑的 test。
# 用法: ./test_otrader.sh <bt|live|build> [0] [test_number]
#   bt    - 回测 test 1-4，默认 1；第二参 0=先 build 再跑
#   live  - 实盘 test 1-3，默认 1；第二参 0=先 build 再跑
#   build - 仅 build 本脚本的 7 个 test 目标（不 make 全量）
set -e

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"
[[ -f "$REPO_ROOT/.env" ]] && set -a && source "$REPO_ROOT/.env" && set +a

# 与 build.sh、test_gtest.sh 共用同一 build 目录
BUILD_DIR="${BUILD_DIR:-Otrader/build}"
TWS_CLIENT_DIR="${REPO_ROOT}/Otrader/thirdparty/IBJts/source/cppclient/client"

# 回测默认（同原 test_backtest.sh）
PARQUET_1="data/SPXW/SPXW-2025-03/20250303.parquet"
PARQUET_4="data/SPXW/SPXW-2025-03/20250303.parquet"
MAX_TIMESTEPS=5
STRATEGY="IvMeanRevertStrategy"
POS_N=30
ORDER_N=5
if [[ "${BACKTEST_LOG}" == "1" || "${BACKTEST_LOG}" == "true" ]]; then
  export BACKTEST_LOG=1
else
  export BACKTEST_LOG=0
fi

# 解析：第一参 = bt|live|build；第二参 = 0 或 test_number；第三参 = test_number（当第二参=0）
MODE="${1:-}"
DO_REBUILD=0
TEST_NUM=""
BUILD_TYPE="${BUILD_TYPE:-Release}"
for a in "$@"; do
  case "$a" in
    debug|d) BUILD_TYPE=Debug ;;
    release|r) BUILD_TYPE=Release ;;
  esac
done
if [[ "$MODE" == "build" ]]; then
  DO_REBUILD=1
elif [[ -n "${2:-}" ]]; then
  if [[ "${2:-}" == "0" ]]; then
    DO_REBUILD=1
    TEST_NUM="${3:-}"
  else
    TEST_NUM="${2:-}"
  fi
fi
[[ -z "$TEST_NUM" && "$MODE" == "bt" ]] && TEST_NUM=1
[[ -z "$TEST_NUM" && "$MODE" == "live" ]] && TEST_NUM=1

if [[ "$MODE" != "bt" && "$MODE" != "live" && "$MODE" != "build" ]]; then
  echo "Usage: $0 <bt|live|build> [0] [test_number]"
  exit 1
fi

# 只 build 本脚本要跑的 test（用聚合 target，避免重复进度输出；依赖 build.sh 已配好 BUILD_TESTS=ON）
do_build() {
  local m="$1"
  if [[ ! -d "$BUILD_DIR" ]]; then
    echo "Error: $BUILD_DIR not found. Run ./build.sh first (shared build dir)."
    exit 1
  fi
  local target=""
  if [[ "$m" == "bt" ]]; then
    target="otrader_script_tests_bt"
  elif [[ "$m" == "live" ]]; then
    target="otrader_script_tests_live"
  else
    target="otrader_script_tests"
  fi
  echo ">>> Building test target $target in $BUILD_DIR"
  ( cd "$BUILD_DIR" && cmake --build . --target "$target" -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 8)" )
  cd "$REPO_ROOT"
}

run_bt() {
  local n="${1:-1}"
  local BT="$BUILD_DIR/tests/non-framwork/backtest"
  case "$n" in
    1) echo ""; echo ">>> Backtest Test 1: test_backtest_data $PARQUET_1 $MAX_TIMESTEPS"
       "$BT/test_backtest_data" "$PARQUET_1" "$MAX_TIMESTEPS" ;;
    2) echo ""; echo ">>> Backtest Test 2: test_backtest (parquet=$PARQUET_1, strategy=$STRATEGY)"
       "$BT/test_backtest" "$PARQUET_1" "$STRATEGY" ;;
    3) echo ""; echo ">>> Backtest Test 3: test_backtest_pos (POS_N=$POS_N)"
       "$BT/test_backtest_pos" "$PARQUET_1" "$STRATEGY" "$POS_N" ;;
    4) echo ""; echo ">>> Backtest Test 4: test_backtest_order (ORDER_N=$ORDER_N)"
       "$BT/test_backtest_order" "$PARQUET_4" "$STRATEGY" "$ORDER_N" ;;
    *) echo "Unknown backtest test: $n (use 1-4)"; exit 1 ;;
  esac
}

run_live() {
  local n="${1:-1}"
  local LV="$BUILD_DIR/tests/non-framwork/live"
  if [[ -d "$TWS_CLIENT_DIR" ]]; then
    export DYLD_LIBRARY_PATH="${TWS_CLIENT_DIR}:${DYLD_LIBRARY_PATH:-}"
    export LD_LIBRARY_PATH="${TWS_CLIENT_DIR}:${LD_LIBRARY_PATH:-}"
  fi
  case "$n" in
    1) echo ""; echo ">>> Live Test 1: test_live_components (WITH TWS connect)"
       "$LV/test_live_components" connect ;;
    2) echo ""; echo ">>> Live Test 2: test_live_market (pull once, ATM call/put for 3 chains)"
       "$LV/test_live_market" ;;
    3) echo ""; echo ">>> Live Test 3: test_live_strategy_straddle (StraddleTestStrategy on 7DTE chain)"
       "$LV/test_live_strategy_straddle" ;;
    *) echo "Unknown live test: $n (use 1-3)"; exit 1 ;;
  esac
}

case "$MODE" in
  build)
    do_build build
    echo ""; echo "Build done."; exit 0
    ;;
  bt)
    do_build bt
    if [[ ! -x "$BUILD_DIR/tests/non-framwork/backtest/test_backtest_data" ]]; then
      echo "Error: backtest tests not built. Run: $0 bt 0 $TEST_NUM"
      exit 1
    fi
    echo "========================================"
    echo "Otrader Backtest BUILD_DIR=$BUILD_DIR test=$TEST_NUM"
    echo "========================================"
    run_bt "$TEST_NUM"
    ;;
  live)
    do_build live
    LV="$BUILD_DIR/tests/non-framwork/live"
    BIN="$LV/test_live_components"
    [[ "$TEST_NUM" == "2" ]] && BIN="$LV/test_live_market"
    [[ "$TEST_NUM" == "3" ]] && BIN="$LV/test_live_strategy_straddle"
    if [[ ! -x "$BIN" ]]; then
      echo "Error: $BIN not built. Run: $0 live 0 $TEST_NUM"
      echo "  Note: live needs libTwsSocketClient in Otrader/thirdparty/IBJts (build there first)."
      exit 1
    fi
    echo "========================================"
    echo "Otrader Live BUILD_DIR=$BUILD_DIR test=$TEST_NUM"
    echo "========================================"
    run_live "$TEST_NUM"
    ;;
  *) echo "Unknown mode: $MODE"; exit 1 ;;
esac

echo ""; echo "Test passed."
