#!/usr/bin/env bash
# Google Test：构建并运行 tests/unit 下所有 GTest 目标（unit/ring/object_pool/live_flow）。与 build.sh、test_otrader.sh 共用 BUILD_DIR。
# 用法: ./test_gtest.sh build | run
#   build - 编所有 GTest 可执行文件（需先 ./build.sh 配好 BUILD_TESTS=ON）
#   run   - 用 ctest 跑全部已发现的用例（若未构建会先 build）
set -e

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$REPO_ROOT"
[[ -f "$REPO_ROOT/.env" ]] && set -a && source "$REPO_ROOT/.env" && set +a

BUILD_DIR="${BUILD_DIR:-Otrader/build}"

do_build() {
  if [[ ! -d "$BUILD_DIR" ]]; then
    echo "Error: $BUILD_DIR not found. Run ./build.sh first (shared build dir)."
    exit 1
  fi
  echo ">>> Building all GTest targets (otrader_gtest_all) in $BUILD_DIR"
  ( cd "$BUILD_DIR" && cmake --build . --target otrader_gtest_all -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 8)" )
  cd "$REPO_ROOT"
}

do_run() {
  if [[ ! -f "$BUILD_DIR/CTestTestfile.cmake" ]]; then
    echo ">>> GTest not configured, building first..."
    do_build
  fi
  echo ">>> Running all tests (ctest)"
  ( cd "$BUILD_DIR" && ctest --output-on-failure "$@" )
}

MODE="${1:-}"
case "$MODE" in
  build)  do_build ;;
  run)    do_run "${@:2}" ;;
  *)      echo "Usage: $0 build | run [ctest args...]"
          echo "  build  - build all GTest executables (run ./build.sh first)"
          echo "  run    - run all tests via ctest (build if needed)"
          exit 1 ;;
esac
