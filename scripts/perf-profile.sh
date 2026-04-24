#!/bin/bash

PERF_CMD="perf"
TARGET_DIR="./build/tests/polyhedral-pass"

# Hardware events specifically chosen for Polyhedral Pass analysis
# L1-dcache: Shows if tiling/interchange improved spatial locality
# LLC: Shows if the problem size fits in cache or is hitting RAM
# dTLB: Shows if memory access patterns are predictable
PERF_EVENTS="L1-dcache-loads,L1-dcache-load-misses,LLC-loads,LLC-load-misses,dTLB-loads,dTLB-load-misses"

echo ""
echo "╔════════════════════╦══════════════════╦══════════════════╦═══════════════╦═══════════════╗"
printf "║ %-18s ║ %16s ║ %16s ║ %13s ║ %13s ║\n" "Test" "L1-dcache (m2r)" "L1-dcache (opt)" "dTLB (m2r)" "dTLB (opt)"
echo "╠════════════════════╬══════════════════╬══════════════════╬═══════════════╬═══════════════╣"

# Process bitcode files
find "$TARGET_DIR" -maxdepth 1 -name "*-m2r.bc" | sort | while read -r base_bc; do
    prefix="${base_bc%-m2r.bc}"
    opt_bc="${prefix}-opt.bc"
    display_name=$(basename "$prefix")

    if [ ! -f "$opt_bc" ]; then
        continue
    fi

    # Run perf and extract counts
    m2r_output=$($PERF_CMD stat -e "$PERF_EVENTS" lli "$base_bc" 2>&1)
    opt_output=$($PERF_CMD stat -e "$PERF_EVENTS" lli "$opt_bc" 2>&1)
    
    m2r_l1dcload=$(echo "$m2r_output" | grep "L1-dcache-loads" | awk '{print $1}' | tr -d ',')
    opt_l1dcload=$(echo "$opt_output" | grep "L1-dcache-loads" | awk '{print $1}' | tr -d ',')
    m2r_l1dcmiss=$(echo "$m2r_output" | grep "L1-dcache-load-misses" | awk '{print $1}' | tr -d ',')
    opt_l1dcmiss=$(echo "$opt_output" | grep "L1-dcache-load-misses" | awk '{print $1}' | tr -d ',')
    m2r_dtlbload=$(echo "$m2r_output" | grep "dTLB-loads" | awk '{print $1}' | tr -d ',')
    opt_dtlbload=$(echo "$opt_output" | grep "dTLB-loads" | awk '{print $1}' | tr -d ',')
    m2r_dtlbmiss=$(echo "$m2r_output" | grep "dTLB-load-misses" | awk '{print $1}' | tr -d ',')
    opt_dtlbmiss=$(echo "$opt_output" | grep "dTLB-load-misses" | awk '{print $1}' | tr -d ',')
    
    # Calculate and output miss percentages directly
    m2r_l1pct=$(awk "BEGIN {printf \"%.2f\", ($m2r_l1dcmiss / $m2r_l1dcload) * 100}")
    opt_l1pct=$(awk "BEGIN {printf \"%.2f\", ($opt_l1dcmiss / $opt_l1dcload) * 100}")
    m2r_dtlbpct=$(awk "BEGIN {printf \"%.2f\", ($m2r_dtlbmiss / $m2r_dtlbload) * 100}")
    opt_dtlbpct=$(awk "BEGIN {printf \"%.2f\", ($opt_dtlbmiss / $opt_dtlbload) * 100}")
    
    printf "║ %-18s ║ %15s%% ║ %15s%% ║ %12s%% ║ %12s%% ║\n" "$display_name" "$m2r_l1pct" "$opt_l1pct" "$m2r_dtlbpct" "$opt_dtlbpct"
done

echo "╚════════════════════╩══════════════════╩══════════════════╩═══════════════╩═══════════════╝"
echo ""