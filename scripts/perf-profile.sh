#!/bin/bash

TARGET_DIR="./build/tests/polyhedral-pass"

HARDWARE_EVENTS="cache-references,cache-misses,instructions,cycles"

CHECK=$(perf stat -e $HARDWARE_EVENTS true 2>&1)

USE_PERF=1
if echo "$CHECK" | grep -q "not supported"; then
    USE_PERF=0
fi

echo ""
echo "════════════════════════════════════════════"
echo " Polyhedral Benchmark (Robust Mode)"
echo " Mode: $([ $USE_PERF -eq 1 ] && echo PERF || echo RUNTIME)"
echo "════════════════════════════════════════════"
echo ""

printf "║ %-18s ║ %-12s ║ %-12s ║\n" "Test" "Baseline" "Optimized"
echo "════════════════════════════════════════════"

find "$TARGET_DIR" -maxdepth 1 -name "*-m2r.bc" | sort | while read -r base_bc; do

    prefix="${base_bc%-m2r.bc}"
    opt_bc="${prefix}-opt.bc"
    name=$(basename "$prefix")

    [ ! -f "$opt_bc" ] && continue

    if [ $USE_PERF -eq 1 ]; then

        m2r_out=$(perf stat -e $HARDWARE_EVENTS lli "$base_bc" 2>&1)
        opt_out=$(perf stat -e $HARDWARE_EVENTS lli "$opt_bc" 2>&1)

        get() {
            echo "$1" | grep "$2" | awk '{print $1}' | tr -d ','
        }

        m2r_refs=$(get "$m2r_out" "cache-references")
        m2r_miss=$(get "$m2r_out" "cache-misses")

        opt_refs=$(get "$opt_out" "cache-references")
        opt_miss=$(get "$opt_out" "cache-misses")

        m2r_refs=${m2r_refs:-1}
        opt_refs=${opt_refs:-1}

        m2r_pct=$(awk "BEGIN {printf \"%.2f\", ($m2r_miss/$m2r_refs)*100}")
        opt_pct=$(awk "BEGIN {printf \"%.2f\", ($opt_miss/$opt_refs)*100}")

        printf "║ %-18s ║ %10s%% ║ %10s%% ║\n" "$name" "$m2r_pct" "$opt_pct"

    else

        # -------- RUNTIME MODE (NO /usr/bin/time) --------

        start1=$(date +%s.%N)
        lli "$base_bc" > /dev/null 2>&1
        end1=$(date +%s.%N)

        start2=$(date +%s.%N)
        lli "$opt_bc" > /dev/null 2>&1
        end2=$(date +%s.%N)

        m2r_time=$(awk "BEGIN {print $end1 - $start1}")
        opt_time=$(awk "BEGIN {print $end2 - $start2}")

        speedup=$(awk "BEGIN {
            if ($opt_time > 0) printf \"%.2f\", $m2r_time / $opt_time;
            else print \"0\"
        }")

        printf "║ %-18s ║ %10ss ║ %10ss ║\n" "$name" "$m2r_time" "$opt_time"
        printf "║ %-18s ║ %-10s ║ %-10s ║\n" "" "speedup:" "$speedup"
    fi

done

echo "════════════════════════════════════════════"
echo ""