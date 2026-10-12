#!/usr/bin/env bash
# The latency benchmarks (engine/tests/bench), in a Release build of their own
# (engine/build/bench, no IB): builds them, runs each, prints what it measured and keeps it in
# engine/build/bench/results/<name>.txt. bench_live_paths and bench_replay replay a day of SPY
# (data/SPY/SPY-2025-03/20250303.parquet, or OPTRA_TEST_DAY).
# Usage: ./bench.sh [name ...]   (bench_live_paths, bench_order_lifecycle, ...; all by default)
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT/engine"

BUILD_DIR="build/bench"
DAY="${OPTRA_TEST_DAY:-$REPO_ROOT/data/SPY/SPY-2025-03/20250303.parquet}"
ALL=(bench_live_paths bench_order_lifecycle bench_event_dispatch bench_object_pool bench_ring
     bench_replay)
if [[ $# -gt 0 ]]; then BENCHES=("$@"); else BENCHES=("${ALL[@]}"); fi

echo ">>> Configuring $BUILD_DIR"
cmake -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++ \
  -DBUILD_BACKTEST=ON -DBUILD_BENCH=ON -DBUILD_TESTS=OFF -DBUILD_LIVE_GRPC=ON \
  -DBUILD_MARKETDATA=ON -DBUILD_GATEWAY=OFF > /dev/null
echo ">>> Building ${BENCHES[*]}"
cmake --build "$BUILD_DIR" -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 8)" \
  --target "${BENCHES[@]}" > /dev/null

mkdir -p "$BUILD_DIR/results"
for b in "${BENCHES[@]}"; do
  exe="$(find "$BUILD_DIR" -name "$b" -type f -executable | head -1)"
  args=()
  [[ "$b" == bench_replay ]] && args=("$DAY")
  echo ">>> $b"
  OPTRA_TEST_DAY="$DAY" "$exe" "${args[@]}" | tee "$BUILD_DIR/results/$b.txt"
done
