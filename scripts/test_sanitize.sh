#!/usr/bin/env bash
# The threaded tests under a sanitizer, in a build of their own (engine/build/sanitize-<kind>, no
# IB): thread = TSAN (data races); address = ASAN + UBSan (memory errors, leaks, undefined
# behaviour). Prints each test's result and how many reports it had; exits 1 on any.
# Usage: ./test_sanitize.sh thread | address
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT/engine"

KIND="${1:-}"
case "$KIND" in
  thread)  FLAGS="-fsanitize=thread" ;;
  address) FLAGS="-fsanitize=address,undefined -fno-sanitize-recover=undefined" ;;
  *)       echo "Usage: $0 thread | address"; exit 1 ;;
esac
BUILD_DIR="build/sanitize-$KIND"

# Every test that runs more than one thread, and the backtest ones the live event loop shares.
TESTS=(
  test_live_event_loop_gtest test_event_loop_safety_gtest test_live_shutdown_e2e_gtest
  test_live_flow_gtest test_live_restore_gtest test_market_data_engine_gtest
  test_event_loop_concurrency_mixed_gtest test_event_loop_burst_gtest
  test_live_connect_gtest test_log_engine_gtest test_mpsc_ring_gtest
  test_object_pool_gtest test_concurrency_design_gtest test_concurrency_shutdown_gtest
  test_booking_cancel_late_fill_gtest test_booking_before_strategy_callbacks_gtest
)

echo ">>> Configuring $BUILD_DIR ($FLAGS)"
cmake -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_CXX_COMPILER=g++ \
  -DBUILD_BACKTEST=ON -DBUILD_TESTS=ON -DBUILD_LIVE_GRPC=ON -DBUILD_MARKETDATA=ON \
  -DBUILD_GATEWAY=OFF "-DCMAKE_CXX_FLAGS=$FLAGS -fno-omit-frame-pointer" \
  "-DCMAKE_EXE_LINKER_FLAGS=$FLAGS" > /dev/null
echo ">>> Building ${#TESTS[@]} tests"
cmake --build "$BUILD_DIR" -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 8)" \
  --target "${TESTS[@]}" > /dev/null

export TSAN_OPTIONS="halt_on_error=0 second_deadlock_stack=1"
export ASAN_OPTIONS="detect_leaks=1 halt_on_error=0"
export UBSAN_OPTIONS="print_stacktrace=1"
failed=0
for t in "${TESTS[@]}"; do
  exe="$(realpath "$(find "$BUILD_DIR" -name "$t" -type f -executable | head -1)")"
  out="$BUILD_DIR/$t.out"
  code=0
  (cd "$(dirname "$exe")" && timeout 900 "$exe" > "$REPO_ROOT/engine/$out" 2>&1) || code=$?
  reports="$(grep -cE "WARNING: ThreadSanitizer|ERROR: AddressSanitizer|ERROR: LeakSanitizer|runtime error:" "$out" || true)"
  echo "$t exit=$code reports=$reports"
  if [[ "$code" != 0 || "$reports" != 0 ]]; then
    failed=1
    echo "    see engine/$out"
  fi
done
exit "$failed"
