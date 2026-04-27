#!/usr/bin/env bash

set -euo pipefail

TARGET_DIR="${1:-./build/benchmarks/tile-32}"
PERF_BIN="${PERF_BIN:-perf}"
PERF_EVENTS="${POLY_PERF_EVENTS:-task-clock,L1-dcache-load-misses,cache-misses,cache-references}"

if [[ ! -d "$TARGET_DIR" ]]; then
  echo "error: '$TARGET_DIR' is not a directory"
  exit 1
fi

run_perf() {
  local executable="$1"
  "$PERF_BIN" stat -x, -e "$PERF_EVENTS" "$executable" 2>&1 >/dev/null
}

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
    $3 == "task-clock" {
      gsub(/[[:space:]]/, "", $1);
      print $1 / 1000;  # convert ms → seconds
      found = 1;
      exit;
    }
    /seconds time elapsed/ {
      gsub(/[[:space:]]/, "", $1);
      print $1;
      found = 1;
      exit;
    }
    END {
      if (!found) print 0;
    }
  ' <<<"$perf_output"
}

ratio() {
  local numerator="$1"
  local denominator="$2"
  awk -v numerator="$numerator" -v denominator="$denominator" 'BEGIN {
    if (denominator == 0) {
      printf "0.00";
    } else {
      printf "%.2f", numerator / denominator;
    }
  }'
}

printf "%-18s %10s %10s %10s %10s %10s %14s %18s\n" \
  "Benchmark" "raw(s)" "licm(s)" "poly(s)" "p/raw" "p/licm" "L1 poly/raw" "cache-miss poly/raw"
printf "%-18s %10s %10s %10s %10s %10s %14s %18s\n" \
  "------------------" "--------" "--------" "--------" "--------" "--------" "--------------" "------------------"

while IFS= read -r raw_exe; do
  prefix="${raw_exe%-raw}"
  licm_exe="${prefix}-licm"
  poly_exe="${prefix}-poly"
  benchmark_name="$(basename "$prefix")"

  if [[ ! -f "$licm_exe" || ! -f "$poly_exe" ]]; then
    continue
  fi

  raw_output="$(run_perf "$raw_exe")"
  licm_output="$(run_perf "$licm_exe")"
  poly_output="$(run_perf "$poly_exe")"

  raw_time="$(extract_elapsed "$raw_output")"
  licm_time="$(extract_elapsed "$licm_output")"
  poly_time="$(extract_elapsed "$poly_output")"

  raw_l1="$(extract_metric "$raw_output" "L1-dcache-load-misses")"
  poly_l1="$(extract_metric "$poly_output" "L1-dcache-load-misses")"
  raw_cache_miss="$(extract_metric "$raw_output" "cache-misses")"
  poly_cache_miss="$(extract_metric "$poly_output" "cache-misses")"

  printf "%-18s %10s %10s %10s %10sx %10sx %14s %18s\n" \
    "$benchmark_name" \
    "$raw_time" \
    "$licm_time" \
    "$poly_time" \
    "$(ratio "$raw_time" "$poly_time")" \
    "$(ratio "$licm_time" "$poly_time")" \
    "${poly_l1}/${raw_l1}" \
    "${poly_cache_miss}/${raw_cache_miss}"
done < <(find "$TARGET_DIR" -maxdepth 1 -type f -name '*-raw' | sort)
