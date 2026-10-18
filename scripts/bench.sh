#!/usr/bin/env bash
# The latency benchmarks (engine/tests/bench), in a Release build of their own
# (engine/build/bench, no IB): builds them, runs each, prints what it measured and keeps it in
# engine/build/bench/results/<name>.txt. bench_live_paths and bench_replay replay a day of SPY
# (data/synthetic/SPY/SPY-2025-03/20250303.parquet, or OPTRA_TEST_DAY).
# Usage: ./bench.sh [--check] [name ...]   (bench_live_paths, bench_order_lifecycle, ...; all by
#        default). --check: run them three times and compare each hot path's median with
#        engine/tests/bench/baseline.tsv (bench_check.cpp; exits 1 on a regression). After an
#        intended change, with the clock fixed: ./bench.sh --check, then ./bench.sh baseline (the
#        last check's medians become the baseline).
#
# A fixed CPU frequency while they run, so that runs compare (left to the governor and turbo boost, the
# same code takes from 1x to 3x as long): `on` turns boost off and holds every core at one frequency
# (2000 MHz by default, about the base clock, which a laptop can hold without heating up); `off` puts
# back what `on` found. Results to compare are taken in the same mode (printed at the top of each).
#        sudo ./bench.sh clock on [MHz] | sudo ./bench.sh clock off | ./bench.sh clock status
# Written for cpufreq with a boost switch (amd-pstate-epp here); `on` refuses a second time until `off`,
# so what it saved is never overwritten.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

CPUS=/sys/devices/system/cpu
BOOST=$CPUS/cpufreq/boost
SAVED=/run/optra-bench-cpu # what `clock on` found, for `clock off`

clock_status() {
  local first=$CPUS/cpu0/cpufreq
  local mhz
  mhz=$(awk '/MHz/ {print int($4)}' /proc/cpuinfo | sort -n | sed -n '1p;$p' | paste -sd- -)
  echo "cpu: $(cat $first/scaling_driver), $(cat $first/scaling_governor)," \
    "epp $(cat $first/energy_performance_preference 2>/dev/null || echo -)," \
    "boost $(cat $BOOST 2>/dev/null || echo -)," \
    "limits $(($(cat $first/scaling_min_freq) / 1000))-$(($(cat $first/scaling_max_freq) / 1000)) MHz," \
    "now $mhz MHz$([[ -f $SAVED ]] && echo ', fixed for benchmarks')"
}

need_root() {
  [[ $(id -u) -eq 0 ]] || { echo "run it with sudo" >&2; exit 1; }
}

clock() {
  case "${1:-status}" in
  on)
    need_root
    [[ -f $SAVED ]] && { echo "already fixed (bench.sh clock off first)" >&2; exit 1; }
    local khz=$((${2:-2000} * 1000)) c lowest
    {
      echo "boost $(cat $BOOST 2>/dev/null || echo -)"
      for c in $CPUS/cpu[0-9]*/cpufreq; do
        echo "cpu $c $(cat $c/scaling_governor) $(cat $c/energy_performance_preference 2>/dev/null || echo -)" \
          "$(cat $c/scaling_min_freq) $(cat $c/scaling_max_freq)"
      done
    } > $SAVED
    [[ -w $BOOST ]] && echo 0 > $BOOST
    for c in $CPUS/cpu[0-9]*/cpufreq; do
      lowest=$(cat $c/cpuinfo_min_freq)
      echo performance > $c/scaling_governor
      echo "$lowest" > $c/scaling_min_freq # below the target first, so max may come down to it
      echo "$khz" > $c/scaling_max_freq
      echo "$khz" > $c/scaling_min_freq
    done
    clock_status
    ;;
  off)
    need_root
    [[ -f $SAVED ]] || { echo "not fixed by bench.sh: nothing to put back" >&2; exit 1; }
    local kind a b c d e
    while read -r kind a b c d e; do
      if [[ $kind == boost && $a != - && -w $BOOST ]]; then
        echo "$a" > $BOOST
      elif [[ $kind == cpu ]]; then
        # a: the cpufreq folder, b: governor, c: epp, d: min, e: max
        echo "$b" > "$a/scaling_governor"
        [[ $c != - ]] && echo "$c" > "$a/energy_performance_preference" 2>/dev/null || true
        echo "$(cat "$a/cpuinfo_min_freq")" > "$a/scaling_min_freq"
        echo "$e" > "$a/scaling_max_freq"
        echo "$d" > "$a/scaling_min_freq"
      fi
    done < $SAVED
    rm -f $SAVED
    clock_status
    ;;
  status)
    clock_status
    ;;
  *)
    echo "usage: $0 clock on [MHz] | off | status" >&2
    exit 2
    ;;
  esac
}

if [[ "${1:-}" == clock ]]; then
  shift
  clock "$@"
  exit 0
fi
if [[ "${1:-}" == baseline ]]; then
  exec "$REPO_ROOT/engine/build/bench/tests/bench/bench_check" --write \
    "$REPO_ROOT"/engine/build/bench/results/runs/rows.*.tsv
fi

cd "$REPO_ROOT/engine"

BUILD_DIR="build/bench"
DAY="${OPTRA_TEST_DAY:-$REPO_ROOT/data/synthetic/SPY/SPY-2025-03/20250303.parquet}"
ALL=(bench_live_paths bench_order_lifecycle bench_event_dispatch bench_object_pool bench_ring
     bench_replay)
CHECK=0
if [[ "${1:-}" == "--check" ]]; then CHECK=1; shift; fi
if [[ $# -gt 0 ]]; then BENCHES=("$@"); else BENCHES=("${ALL[@]}"); fi

echo ">>> Configuring $BUILD_DIR"
cmake -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++ \
  -DBUILD_BACKTEST=ON -DBUILD_BENCH=ON -DBUILD_TESTS=OFF -DBUILD_LIVE_GRPC=ON \
  -DBUILD_MARKETDATA=ON -DBUILD_GATEWAY=OFF > /dev/null
echo ">>> Building ${BENCHES[*]}"
cmake --build "$BUILD_DIR" -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 8)" \
  --target "${BENCHES[@]}" bench_check > /dev/null

mkdir -p "$BUILD_DIR/results/runs"
rm -f "$BUILD_DIR"/results/runs/rows.*.tsv
# With --check, each run's rows also go to runs/rows.<run>.tsv for bench_check (bench_util.hpp).
[[ "$CHECK" -eq 1 ]] && export OPTRA_BENCH_ROWS="$PWD/$BUILD_DIR/results/runs/rows.1.tsv"
CPU_STATE="$(clock_status)" # the conditions, at the top of each result
header() {
  echo "measured: $(date '+%Y-%m-%d %H:%M:%S %Z')"
  echo "event thread: OPTRA_EVENT_SPIN_US=${OPTRA_EVENT_SPIN_US:-unset} OPTRA_EVENT_CPU=${OPTRA_EVENT_CPU:-unset}"
  echo "$CPU_STATE"
}
for b in "${BENCHES[@]}"; do
  exe="$(find "$BUILD_DIR" -name "$b" -type f -executable | head -1)"
  args=()
  [[ "$b" == bench_replay ]] && args=("$DAY")
  echo ">>> $b"
  { header; OPTRA_TEST_DAY="$DAY" "$exe" "${args[@]}"; } | tee "$BUILD_DIR/results/$b.txt"
done

if [[ "$CHECK" -eq 1 ]]; then
  # One run on a shared machine varies too much: two more, and each row's median is compared.
  for run in 2 3; do
    export OPTRA_BENCH_ROWS="$PWD/$BUILD_DIR/results/runs/rows.$run.tsv"
    for b in "${BENCHES[@]}"; do
      exe="$(find "$BUILD_DIR" -name "$b" -type f -executable | head -1)"
      args=()
      [[ "$b" == bench_replay ]] && args=("$DAY")
      { header; OPTRA_TEST_DAY="$DAY" "$exe" "${args[@]}"; } > "$BUILD_DIR/results/runs/$b.$run.txt"
    done
  done
  echo ">>> Compared with the baseline (median of three runs)"
  "$BUILD_DIR/tests/bench/bench_check" "$BUILD_DIR"/results/runs/rows.*.tsv
fi
