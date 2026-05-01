#!/bin/bash

TARGET_DIR="./build/benchmarks"

if [ ! -d "$TARGET_DIR" ]; then
    echo "Error: '$TARGET_DIR' is not a valid directory."
    exit 1
fi

instr_count() {
    local bc_file="$1"
    local stats
    stats=$(lli -stats -force-interpreter "$bc_file" 2>&1 >/dev/null)
    echo "$stats" | grep "instructions executed" | awk '{print $1}' | tr -d ',' | head -n 1
}

percent_diff() {
    local base="$1"
    local candidate="$2"
    if [ "$base" -gt 0 ]; then
        awk "BEGIN {printf \"%.2f\", (($base - $candidate) / $base) * 100}"
    else
        printf "n/a"
    fi
}

echo ""
printf "%-18s %14s %14s %14s %14s %12s %12s %12s\n" \
    "Benchmark" "raw instr" "licm instr" "lcm instr" "poly instr" \
    "poly/raw%" "poly/licm%" "poly/lcm%"
printf "%-18s %14s %14s %14s %14s %12s %12s %12s\n" \
    "------------------" "--------------" "--------------" "--------------" "--------------" \
    "------------" "------------" "------------"

find "$TARGET_DIR" -maxdepth 1 -name "*-raw.bc" | sort | while read -r raw_bc; do
    prefix="${raw_bc%-raw.bc}"
    licm_bc="${prefix}-licm.bc"
    lcm_bc="${prefix}-lcm.bc"
    poly_bc="${prefix}-poly.bc"
    name=$(basename "$prefix")

    [ ! -f "$licm_bc" ] && continue
    [ ! -f "$lcm_bc" ] && continue
    [ ! -f "$poly_bc" ] && continue

    raw_count=$(instr_count "$raw_bc")
    licm_count=$(instr_count "$licm_bc")
    lcm_count=$(instr_count "$lcm_bc")
    poly_count=$(instr_count "$poly_bc")

    raw_count=${raw_count:-0}
    licm_count=${licm_count:-0}
    lcm_count=${lcm_count:-0}
    poly_count=${poly_count:-0}

    poly_raw=$(percent_diff "$raw_count" "$poly_count")
    poly_licm=$(percent_diff "$licm_count" "$poly_count")
    poly_lcm=$(percent_diff "$lcm_count" "$poly_count")

    printf "%-18s %14s %14s %14s %14s %12s %12s %12s\n" \
        "$name" "$raw_count" "$licm_count" "$lcm_count" "$poly_count" \
        "$poly_raw" "$poly_licm" "$poly_lcm"
done

echo ""
