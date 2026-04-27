#!/usr/bin/env bash

set -euo pipefail

TARGET_DIR="${1:-./build/benchmarks/tile-32}"

if [[ ! -d "$TARGET_DIR" ]]; then
  echo "error: '$TARGET_DIR' is not a directory"
  exit 1
fi

measure_instructions() {
  local bitcode_file="$1"
  local stats

  stats=$(lli -stats -force-interpreter "$bitcode_file" 2>&1 >/dev/null)
  awk '/instructions executed/ {gsub(/,/, "", $1); print $1; found=1; exit} END {if (!found) print 0}' <<<"$stats"
}

percent_delta() {
  local baseline="$1"
  local candidate="$2"
  awk -v baseline="$baseline" -v candidate="$candidate" 'BEGIN {
    if (baseline == 0) {
      printf "n/a";
    } else {
      printf "%.2f", ((baseline - candidate) / baseline) * 100.0;
    }
  }'
}

printf "%-18s %14s %14s %14s %12s %12s\n" \
  "Benchmark" "raw instr" "licm instr" "poly instr" "poly/raw%" "poly/licm%"
printf "%-18s %14s %14s %14s %12s %12s\n" \
  "------------------" "--------------" "--------------" "--------------" "------------" "------------"

while IFS= read -r raw_bc; do
  prefix="${raw_bc%-raw.bc}"
  licm_bc="${prefix}-licm.bc"
  poly_bc="${prefix}-poly.bc"
  benchmark_name="$(basename "$prefix")"

  if [[ ! -f "$licm_bc" || ! -f "$poly_bc" ]]; then
    continue
  fi

  raw_count="$(measure_instructions "$raw_bc")"
  licm_count="$(measure_instructions "$licm_bc")"
  poly_count="$(measure_instructions "$poly_bc")"

  printf "%-18s %14s %14s %14s %12s %12s\n" \
    "$benchmark_name" \
    "$raw_count" \
    "$licm_count" \
    "$poly_count" \
    "$(percent_delta "$raw_count" "$poly_count")" \
    "$(percent_delta "$licm_count" "$poly_count")"
done < <(find "$TARGET_DIR" -maxdepth 1 -type f -name '*-raw.bc' | sort)
