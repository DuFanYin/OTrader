#!/usr/bin/env bash
# Build and run the concurrency benchmarks; prints Markdown tables.
#   ./run.sh            full matrix
#   QUICK=1 ./run.sh    smaller sizes
# Every LSM configuration runs in its own process so peak RSS is per backend.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD="$HERE/build"
cmake -S "$HERE" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$BUILD" -j >/dev/null

NCPU="$(getconf _NPROCESSORS_ONLN)"
if [[ "${QUICK:-0}" == "1" ]]; then
  PATHS=(50000); THREADS=(1 "$NCPU"); MESSAGES=2000000; REPEAT=3
else
  PATHS=(50000 200000 500000); THREADS=(1 2 4 "$NCPU"); MESSAGES=10000000; REPEAT=5
fi

field() { sed -E "s/.*\"$1\":\"?([^,\"}]*).*/\1/"; }

echo "## Queue: 1 producer / 1 consumer, $MESSAGES uint64 messages, capacity 4096"
echo
echo "| impl | ms | msgs/s | in-order check |"
echo "|---|---:|---:|---|"
for impl in spsc locked; do
  line="$("$BUILD/queue_bench" --impl "$impl" --messages "$MESSAGES")"
  printf "| %s | %s | %s | %s |\n" "$impl" "$(field ms <<<"$line")" "$(field msgs_per_sec <<<"$line")" "$(field ok <<<"$line")"
done
echo

echo "## LSM American put (S=K=100, r=5%, sigma=20%, T=1, 50 exercise dates), best of $REPEAT"
echo
echo "| paths | backend | threads | best ms | median ms | speed-up vs serial | peak RSS MiB | price |"
echo "|---:|---|---:|---:|---:|---:|---:|---|"
status=0
for p in "${PATHS[@]}"; do
  serial="$("$BUILD/lsm_bench" --backend serial --paths "$p" --repeat "$REPEAT")"
  s_ms="$(field best_ms <<<"$serial")"; ref_price="$(field price <<<"$serial")"
  printf "| %s | serial | 1 | %s | %s | 1.00 | %s | %s |\n" "$p" "$s_ms" "$(field median_ms <<<"$serial")" \
    "$(field peak_rss_mib <<<"$serial")" "$ref_price"
  for backend in omp pool; do
    for t in "${THREADS[@]}"; do
      line="$("$BUILD/lsm_bench" --backend "$backend" --threads "$t" --paths "$p" --repeat "$REPEAT")"
      ms="$(field best_ms <<<"$line")"; price="$(field price <<<"$line")"
      same="$([[ "$price" == "$ref_price" ]] && echo "identical" || { status=1; echo "MISMATCH $price"; })"
      printf "| %s | %s | %s | %s | %s | %.2f | %s | %s |\n" "$p" "$backend" "$t" "$ms" "$(field median_ms <<<"$line")" \
        "$(echo "$s_ms / $ms" | bc -l)" "$(field peak_rss_mib <<<"$line")" "$same"
    done
  done
done
exit $status
