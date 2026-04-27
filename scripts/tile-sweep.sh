#!/usr/bin/env bash

set -euo pipefail

MAKE_BIN="${MAKE_BIN:-make}"
PERF_BIN="${PERF_BIN:-perf}"
PERF_EVENTS="${POLY_PERF_EVENTS:-L1-dcache-load-misses,cache-misses,cache-references}"
REPORT_DIR="./build/reports"
RESULT_CSV="$REPORT_DIR/tile-sweep.csv"

if [[ "$#" -gt 0 ]]; then
  TILE_SIZES=("$@")
else
  TILE_SIZES=(8 16 32 64)
fi

mkdir -p "$REPORT_DIR"
printf "benchmark,tile_size,elapsed_s,l1_dcache_load_misses,cache_misses\n" > "$RESULT_CSV"

extract_metric() {
  local perf_output="$1"
  local metric_name="$2"
  awk -F, -v metric="$metric_name" '
    $3 == metric {
      gsub(/[[:space:]]/, "", $1);
      print $1;
      found = 1;
      exit;
    }
    END {
      if (!found) {
        print 0;
      }
    }
  ' <<<"$perf_output"
}

extract_elapsed() {
  local perf_output="$1"
  awk -F, '
    /seconds time elapsed/ {
      gsub(/[[:space:]]/, "", $1);
      print $1;
      found = 1;
      exit;
    }
    END {
      if (!found) {
        print 0;
      }
    }
  ' <<<"$perf_output"
}

for tile in "${TILE_SIZES[@]}"; do
  "$MAKE_BIN" benchmarks TILE_SIZE="$tile" >/dev/null
  target_dir="./build/benchmarks/tile-$tile"

  while IFS= read -r poly_exe; do
    benchmark_name="$(basename "$poly_exe" -poly)"
    perf_output="$("$PERF_BIN" stat -x, -e "$PERF_EVENTS" "$poly_exe" 2>&1 >/dev/null)"
    elapsed_s="$(extract_elapsed "$perf_output")"
    l1_misses="$(extract_metric "$perf_output" "L1-dcache-load-misses")"
    cache_misses="$(extract_metric "$perf_output" "cache-misses")"
    printf "%s,%s,%s,%s,%s\n" \
      "$benchmark_name" "$tile" "$elapsed_s" "$l1_misses" "$cache_misses" >> "$RESULT_CSV"
  done < <(find "$target_dir" -maxdepth 1 -type f -name '*-poly' | sort)
done

echo "wrote $RESULT_CSV"
awk -F, '
  NR == 1 {
    next;
  }
  {
    if (!($1 in best_runtime) || ($3 + 0.0) < best_runtime[$1]) {
      best_runtime[$1] = $3 + 0.0;
      best_tile[$1] = $2;
    }
  }
  END {
    for (benchmark in best_runtime) {
      printf "%-18s tile=%-4s runtime=%.6f s\n",
             benchmark, best_tile[benchmark], best_runtime[benchmark];
    }
  }
' "$RESULT_CSV" | sort
