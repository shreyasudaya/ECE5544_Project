#!/bin/bash

TARGET_DIR="./build/tests/polyhedral-pass"

# Check if the provided path is actually a directory
if [ ! -d "$TARGET_DIR" ]; then
    echo "Error: '$TARGET_DIR' is not a valid directory."
    exit 1
fi

echo ""
echo "╔════════════════════╦═══════════════════╦═══════════════════╦═══════════════════╗"
printf "║ %-18s ║ %17s ║ %17s ║ %17s ║\n" "Test" "m2r (instr)" "opt (instr)" "Diff (%)"
echo "╠════════════════════╬═══════════════════╬═══════════════════╬═══════════════════╣"

# Find all files ending in -m2r.bc
find "$TARGET_DIR" -maxdepth 1 -name "*-m2r.bc" | sort | while read -r base_bc; do
    
    # Identify the prefix (everything before -m2r.bc)
    prefix="${base_bc%-m2r.bc}"
    opt_bc="${prefix}-opt.bc"
    
    # Extract clean filename for display
    display_name=$(basename "$prefix")

    # Check if the optimized counterpart exists
    if [ ! -f "$opt_bc" ]; then
        continue
    fi

    # Run lli with interpreter and stats
    # 2>&1 merges stderr (where stats live) into stdout so we can grep it
    m2r_stats=$(lli -stats -force-interpreter "$base_bc" 2>&1 >/dev/null)
    opt_stats=$(lli -stats -force-interpreter "$opt_bc" 2>&1 >/dev/null)

    # Extract the digit count
    m2r_count=$(echo "$m2r_stats" | grep "instructions executed" | awk '{print $1}' | tr -d ',')
    opt_count=$(echo "$opt_stats" | grep "instructions executed" | awk '{print $1}' | tr -d ',')

    # Default to 0 if extraction fails
    m2r_count=${m2r_count:-0}
    opt_count=${opt_count:-0}

    # Calculate difference (positive means opt is better/smaller)
    diff=$((m2r_count - opt_count))

    # Calculate percentage using awk for float precision, avoiding divide-by-zero
    if [ "$m2r_count" -gt 0 ] && [ "$diff" -gt 0 ]; then
        percent=$(awk "BEGIN {printf \"%.2f\", ($diff / $m2r_count) * 100}")
        diff_display="${diff} (+${percent}%)"
    else
        diff_display="${diff}"
    fi

    # Colorize the output: Red if opt is higher (regression), Green if lower
    if [ "$diff" -lt 0 ]; then
        # Red text for regression
        printf "║ %-18s ║ %17s ║ %17s ║ \e[31m%17s\e[0m ║\n" "$display_name" "$m2r_count" "$opt_count" "$diff_display"
    else
        # Green text for improvement
        printf "║ %-18s ║ %17s ║ %17s ║ \e[32m%17s\e[0m ║\n" "$display_name" "$m2r_count" "$opt_count" "$diff_display"
    fi
done

echo "╚════════════════════╩═══════════════════╩═══════════════════╩═══════════════════╝"
echo ""