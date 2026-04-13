#!/bin/bash

CHAMPSIM_BIN="./bin/champsim"
TRACE_DIR="./Traces"
SIMPOINT_ROOT="./Weights"

WARMUP=50000000
SIM=200000000

echo "========================================"
echo "Starting ChampSim batch run"
echo "========================================"

# If benchmarks are passed as arguments, use them; otherwise run all
if [[ $# -gt 0 ]]; then
    BENCHMARKS=("$@")
else
    BENCHMARKS=()
    for dir in "$SIMPOINT_ROOT"/*/; do
        BENCHMARKS+=("$(basename "$dir")")
    done
fi

# Loop over selected benchmarks
for bench_name in "${BENCHMARKS[@]}"; do

    bench_dir="${SIMPOINT_ROOT}/${bench_name}"
    sim_file="${bench_dir}/simpoints.out"
    weight_file="${bench_dir}/weights.out"

    if [[ ! -f "$sim_file" || ! -f "$weight_file" ]]; then
        echo "Skipping $bench_name (missing files)"
        continue
    fi

    echo "----------------------------------------"
    echo "Benchmark: $bench_name"
    echo "----------------------------------------"

    total_weight=0
    weighted_acc=0
    weighted_mpki=0
    weighted_ipc=0

    # Read both files line-by-line together
    exec 3< "$weight_file"

    while read -r sp_line && read -r weight <&3; do

        sp=$(echo "$sp_line" | awk '{print $1}')

        # Find matching trace
        trace=$(ls "$TRACE_DIR"/${bench_name}-${sp}B.champsimtrace.xz 2>/dev/null)

        if [[ -z "$trace" ]]; then
            echo "Skipping simpoint $sp (no trace found)"
            continue
        fi

        echo "Running: $(basename "$trace") (weight=$weight)"

        output=$($CHAMPSIM_BIN \
            --warmup-instructions $WARMUP \
            --simulation-instructions $SIM \
            "$trace")

        # Extract metrics
        acc=$(echo "$output" | grep "Branch Prediction Accuracy" | awk '{print $6}' | tr -d '%')
        mpki=$(echo "$output" | grep "Branch Prediction Accuracy" | awk '{print $8}')

        # Extract cumulative IPC (from final stats section)
        ipc=$(echo "$output" | grep "CPU 0 cumulative IPC:" | tail -n 1 | awk '{print $5}')

        # Safety check
        if [[ -z "$ipc" ]]; then
            echo "Warning: IPC not found for $trace"
            continue
        fi

        # Accumulate weighted values
        total_weight=$(echo "$total_weight + $weight" | bc -l)
        weighted_acc=$(echo "$weighted_acc + ($weight * $acc)" | bc -l)
        weighted_mpki=$(echo "$weighted_mpki + ($weight * $mpki)" | bc -l)
        weighted_ipc=$(echo "$weighted_ipc + ($weight * $ipc)" | bc -l)

    done < "$sim_file"

    exec 3<&-

    # Handle case where nothing ran
    if (( $(echo "$total_weight == 0" | bc -l) )); then
        echo "No valid traces for $bench_name"
        continue
    fi

    final_acc=$(echo "$weighted_acc / $total_weight" | bc -l)
    final_mpki=$(echo "$weighted_mpki / $total_weight" | bc -l)
    final_ipc=$(echo "$weighted_ipc / $total_weight" | bc -l)

    echo "====== $bench_name RESULTS ======"
    echo "Weighted Accuracy: $final_acc %"
    echo "Weighted MPKI: $final_mpki"
    echo "Weighted IPC: $final_ipc"
    echo "================================"

done
