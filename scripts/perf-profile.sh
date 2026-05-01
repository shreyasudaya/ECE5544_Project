#!/bin/bash

TARGET_DIR="./build/benchmarks"
HARDWARE_EVENTS="cache-references,cache-misses"

if [ ! -d "$TARGET_DIR" ]; then
    echo "Error: '$TARGET_DIR' is not a valid directory."
    exit 1
fi

CHECK=$(perf stat -e $HARDWARE_EVENTS true 2>&1)

USE_PERF=1
if echo "$CHECK" | grep -Eqi "not supported|no permission|permission denied|cannot find"; then
    USE_PERF=0
fi

runtime_of() {
    local bc_file="$1"
    local start end
    start=$(date +%s.%N)
    lli "$bc_file" >/dev/null 2>&1
    end=$(date +%s.%N)
    awk "BEGIN {printf \"%.5f\", $end - $start}"
}

speedup_of() {
    local base="$1"
    local candidate="$2"
    awk "BEGIN {if ($candidate > 0) printf \"%.2fx\", $base / $candidate; else print \"n/a\"}"
}

cache_pct_of() {
    local bc_file="$1"
    local out refs misses
    out=$(perf stat -e $HARDWARE_EVENTS lli "$bc_file" 2>&1 >/dev/null)
    refs=$(echo "$out" | grep "cache-references" | awk '{print $1}' | tr -d ',' | head -n 1)
    misses=$(echo "$out" | grep "cache-misses" | awk '{print $1}' | tr -d ',' | head -n 1)
    if ! [[ "$refs" =~ ^[0-9]+$ ]] || ! [[ "$misses" =~ ^[0-9]+$ ]]; then
        printf "n/a"
    elif [ "$refs" -gt 0 ]; then
        awk "BEGIN {printf \"%.2f%%\", ($misses / $refs) * 100}"
    else
        printf "n/a"
    fi
}

echo ""
echo "Mode: $([ $USE_PERF -eq 1 ] && echo PERF || echo RUNTIME)"
echo ""

if [ $USE_PERF -eq 1 ]; then
    printf "%-18s %12s %12s %12s %12s\n" \
        "Benchmark" "raw miss%" "licm miss%" "lcm miss%" "poly miss%"
    printf "%-18s %12s %12s %12s %12s\n" \
        "------------------" "------------" "------------" "------------" "------------"
else
    printf "%-18s %10s %10s %10s %10s %10s %10s %10s\n" \
        "Benchmark" "raw(s)" "licm(s)" "lcm(s)" "poly(s)" \
        "p/raw" "p/licm" "p/lcm"
    printf "%-18s %10s %10s %10s %10s %10s %10s %10s\n" \
        "------------------" "--------" "--------" "--------" "--------" \
        "--------" "--------" "--------"
fi

find "$TARGET_DIR" -maxdepth 1 -name "*-raw.bc" | sort | while read -r raw_bc; do
    prefix="${raw_bc%-raw.bc}"
    licm_bc="${prefix}-licm.bc"
    lcm_bc="${prefix}-lcm.bc"
    poly_bc="${prefix}-poly.bc"
    name=$(basename "$prefix")

    [ ! -f "$licm_bc" ] && continue
    [ ! -f "$lcm_bc" ] && continue
    [ ! -f "$poly_bc" ] && continue

    if [ $USE_PERF -eq 1 ]; then
        raw_pct=$(cache_pct_of "$raw_bc")
        licm_pct=$(cache_pct_of "$licm_bc")
        lcm_pct=$(cache_pct_of "$lcm_bc")
        poly_pct=$(cache_pct_of "$poly_bc")

        printf "%-18s %12s %12s %12s %12s\n" \
            "$name" "$raw_pct" "$licm_pct" "$lcm_pct" "$poly_pct"
    else
        raw_time=$(runtime_of "$raw_bc")
        licm_time=$(runtime_of "$licm_bc")
        lcm_time=$(runtime_of "$lcm_bc")
        poly_time=$(runtime_of "$poly_bc")

        p_raw=$(speedup_of "$raw_time" "$poly_time")
        p_licm=$(speedup_of "$licm_time" "$poly_time")
        p_lcm=$(speedup_of "$lcm_time" "$poly_time")

        printf "%-18s %10s %10s %10s %10s %10s %10s %10s\n" \
            "$name" "$raw_time" "$licm_time" "$lcm_time" "$poly_time" \
            "$p_raw" "$p_licm" "$p_lcm"
    fi
done

echo ""
