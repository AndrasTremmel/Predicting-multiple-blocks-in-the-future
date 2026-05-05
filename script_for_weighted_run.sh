# #!/bin/bash

# CHAMPSIM_BIN="./bin/champsim"
# TRACE_DIR="./Traces"
# SIMPOINT_ROOT="./Weights"

# WARMUP=10000
# SIM=100000

# echo "========================================"
# echo "Starting ChampSim batch run"
# echo "========================================"

# # Default SPEC CPU2017 benchmark list
# SPEC2017_BENCHMARKS=(
#     600.perlbench_s 602.gcc_s 603.bwaves_s 605.mcf_s
#     607.cactuBSSN_s 619.lbm_s 620.omnetpp_s 621.wrf_s
#     623.xalancbmk_s 625.x264_s 627.cam4_s 628.pop2_s
#     631.deepsjeng_s 638.imagick_s 641.leela_s 644.nab_s
#     648.exchange2_s 649.fotonik3d_s 654.roms_s 657.xz_s
# )

# # If benchmarks are passed as arguments, use them; otherwise use SPEC2017
# if [[ $# -gt 0 ]]; then
#     BENCHMARKS=("$@")
# else
#     BENCHMARKS=("${SPEC2017_BENCHMARKS[@]}")
# fi

# # Loop over selected benchmarks
# for bench_name in "${BENCHMARKS[@]}"; do

#     bench_dir="${SIMPOINT_ROOT}/${bench_name}"
#     sim_file="${bench_dir}/simpoints.out"
#     weight_file="${bench_dir}/weights.out"

#     if [[ ! -f "$sim_file" || ! -f "$weight_file" ]]; then
#         echo "Skipping $bench_name (missing files)"
#         continue
#     fi

#     echo "----------------------------------------"
#     echo "Benchmark: $bench_name"
#     echo "----------------------------------------"

#     total_weight=0
#     weighted_acc=0
#     weighted_mpki=0
#     weighted_ipc=0

#     # Read both files line-by-line together
#     exec 3< "$weight_file"

#     while read -r sp_line && read -r weight <&3; do

#         sp=$(echo "$sp_line" | awk '{print $1}')

#         # Find matching trace
#         trace=$(ls "$TRACE_DIR"/${bench_name}-${sp}B.champsimtrace.xz 2>/dev/null)

#         if [[ -z "$trace" ]]; then
#             echo "Skipping simpoint $sp (no trace found)"
#             continue
#         fi

#         echo "Running: $(basename "$trace") (weight=$weight)"

#         output=$($CHAMPSIM_BIN \
#             --warmup-instructions $WARMUP \
#             --simulation-instructions $SIM \
#             "$trace")

#         # Extract metrics
#         acc=$(echo "$output" | grep "Branch Prediction Accuracy" | awk '{print $6}' | tr -d '%')
#         mpki=$(echo "$output" | grep "Branch Prediction Accuracy" | awk '{print $8}')

#         # Extract cumulative IPC (from final stats section)
#         ipc=$(echo "$output" | grep "CPU 0 cumulative IPC:" | tail -n 1 | awk '{print $5}')

#         # Safety check
#         if [[ -z "$ipc" ]]; then
#             echo "Warning: IPC not found for $trace"
#             continue
#         fi

#         # Accumulate weighted values
#         total_weight=$(echo "$total_weight + $weight" | bc -l)
#         weighted_acc=$(echo "$weighted_acc + ($weight * $acc)" | bc -l)
#         weighted_mpki=$(echo "$weighted_mpki + ($weight * $mpki)" | bc -l)
#         weighted_ipc=$(echo "$weighted_ipc + ($weight * $ipc)" | bc -l)

#     done < "$sim_file"

#     exec 3<&-

#     # Handle case where nothing ran
#     if (( $(echo "$total_weight == 0" | bc -l) )); then
#         echo "No valid traces for $bench_name"
#         continue
#     fi

#     final_acc=$(echo "$weighted_acc / $total_weight" | bc -l)
#     final_mpki=$(echo "$weighted_mpki / $total_weight" | bc -l)
#     final_ipc=$(echo "$weighted_ipc / $total_weight" | bc -l)

#     echo "====== $bench_name RESULTS ======"
#     echo "Weighted Accuracy: $final_acc %"
#     echo "Weighted MPKI: $final_mpki"
#     echo "Weighted IPC: $final_ipc"
#     echo "================================"

# done


#!/bin/bash

# ── Configuration ───────────────────────────────────────────────────────────
CHAMPSIM_BIN="./bin/champsim"
TRACE_DIR="./Traces"
SIMPOINT_ROOT="./Weights"

WARMUP=10000
SIM=100000

# ── Robustness checks ───────────────────────────────────────────────────────
if [[ ! -x "$CHAMPSIM_BIN" ]]; then
    echo "ERROR: ChampSim binary not found or not executable: $CHAMPSIM_BIN"
    exit 1
fi

if [[ ! -d "$TRACE_DIR" ]]; then
    echo "ERROR: Trace directory not found: $TRACE_DIR"
    exit 1
fi

if [[ ! -d "$SIMPOINT_ROOT" ]]; then
    echo "ERROR: Simpoint directory not found: $SIMPOINT_ROOT"
    exit 1
fi

echo "========================================"
echo "Starting ChampSim batch run"
echo "========================================"

# ── Benchmark list ──────────────────────────────────────────────────────────
SPEC2017_BENCHMARKS=(
    600.perlbench_s 602.gcc_s 603.bwaves_s 605.mcf_s
    607.cactuBSSN_s 619.lbm_s 620.omnetpp_s 621.wrf_s
    623.xalancbmk_s 625.x264_s 627.cam4_s 628.pop2_s
    631.deepsjeng_s 638.imagick_s 641.leela_s 644.nab_s
    648.exchange2_s 649.fotonik3d_s 654.roms_s 657.xz_s
)

if [[ $# -gt 0 ]]; then
    BENCHMARKS=("$@")
else
    BENCHMARKS=("${SPEC2017_BENCHMARKS[@]}")
fi

# ── Overall accumulators ────────────────────────────────────────────────────
overall_total_weight=0
overall_weighted_acc=0
overall_weighted_mpki=0
overall_weighted_ipc=0

# ── Main benchmark loop ─────────────────────────────────────────────────────
for bench_name in "${BENCHMARKS[@]}"; do

    bench_dir="${SIMPOINT_ROOT}/${bench_name}"
    sim_file="${bench_dir}/simpoints.out"
    weight_file="${bench_dir}/weights.out"

    if [[ ! -f "$sim_file" || ! -f "$weight_file" ]]; then
        echo "Skipping $bench_name (missing simpoint or weight file)"
        continue
    fi

    echo ""
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
        trace=$(ls "$TRACE_DIR"/${bench_name}-${sp}B.champsimtrace.xz 2>/dev/null | head -n 1)

        if [[ -z "$trace" ]]; then
            echo "Skipping simpoint $sp (no trace found)"
            continue
        fi

        echo "Running: $(basename "$trace") (weight=$weight)"

        # ── Run simulation, capture output, then print everything ─────────────
        output=$("$CHAMPSIM_BIN" \
            --warmup-instructions "$WARMUP" \
            --simulation-instructions "$SIM" \
            "$trace" 2>&1)

        echo "$output"

        # ── Extract metrics (robust, with fallbacks) ──────────────────────────
        acc=$(echo "$output" | grep "Branch Prediction Accuracy" | awk '{print $6}' | tr -d '%')
        mpki=$(echo "$output" | grep "Branch Prediction Accuracy" | awk '{print $8}')
        ipc=$(echo "$output" | grep "CPU 0 cumulative IPC:" | tail -n 1 | awk '{print $5}')

        # Fallback 1: looser grep if the exact string is missing
        if [[ -z "$acc" ]]; then
            acc=$(echo "$output" | grep -i "accuracy" | tail -n 1 | awk '{for(i=1;i<=NF;i++) if($i ~ /^[0-9]+\.?[0-9]*%?$/) {gsub(/%/,"",$i); print $i; exit}}')
        fi
        if [[ -z "$mpki" ]]; then
            mpki=$(echo "$output" | grep -i "mpki" | tail -n 1 | awk '{for(i=1;i<=NF;i++) if($i ~ /^[0-9]+\.?[0-9]*$/) {print $i; exit}}')
        fi
        if [[ -z "$ipc" ]]; then
            ipc=$(echo "$output" | grep -i "cumulative IPC" | tail -n 1 | awk '{for(i=1;i<=NF;i++) if($i ~ /^[0-9]+\.?[0-9]*$/) {print $i; exit}}')
        fi

        # Safety check
        if [[ -z "$ipc" || -z "$acc" || -z "$mpki" ]]; then
            echo "Warning: Missing metrics for $trace (acc=$acc, mpki=$mpki, ipc=$ipc)"
            continue
        fi

        # ── Accumulate weighted values ──────────────────────────────────────
        total_weight=$(echo "$total_weight + $weight" | bc -l)
        weighted_acc=$(echo "$weighted_acc + ($weight * $acc)" | bc -l)
        weighted_mpki=$(echo "$weighted_mpki + ($weight * $mpki)" | bc -l)
        weighted_ipc=$(echo "$weighted_ipc + ($weight * $ipc)" | bc -l)

    done < "$sim_file"

    exec 3<&-

    # ── Per-benchmark summary ───────────────────────────────────────────────
    if (( $(echo "$total_weight == 0" | bc -l) )); then
        echo "No valid traces for $bench_name"
        continue
    fi

    final_acc=$(echo "$weighted_acc / $total_weight" | bc -l)
    final_mpki=$(echo "$weighted_mpki / $total_weight" | bc -l)
    final_ipc=$(echo "$weighted_ipc / $total_weight" | bc -l)

    echo "====== $bench_name RESULTS ======"
    printf "Weighted Accuracy: %.4f %%\n" "$final_acc"
    printf "Weighted MPKI:     %.4f\n"      "$final_mpki"
    printf "Weighted IPC:      %.4f\n"      "$final_ipc"
    echo "=================================="

    # Accumulate overall
    overall_total_weight=$(echo "$overall_total_weight + $total_weight" | bc -l)
    overall_weighted_acc=$(echo "$overall_weighted_acc + $weighted_acc" | bc -l)
    overall_weighted_mpki=$(echo "$overall_weighted_mpki + $weighted_mpki" | bc -l)
    overall_weighted_ipc=$(echo "$overall_weighted_ipc + $weighted_ipc" | bc -l)

done

# ── Overall weighted summary ────────────────────────────────────────────────
echo ""

if (( $(echo "$overall_total_weight > 0" | bc -l) )); then
    overall_acc=$(echo "$overall_weighted_acc / $overall_total_weight" | bc -l)
    overall_mpki=$(echo "$overall_weighted_mpki / $overall_total_weight" | bc -l)
    overall_ipc=$(echo "$overall_weighted_ipc / $overall_total_weight" | bc -l)

    echo "########################################"
    echo "####### OVERALL WEIGHTED SUMMARY #######"
    echo "########################################"
    printf "Overall Weighted Accuracy: %.4f %%\n" "$overall_acc"
    printf "Overall Weighted MPKI:     %.4f\n"      "$overall_mpki"
    printf "Overall Weighted IPC:      %.4f\n"      "$overall_ipc"
    echo "########################################"
else
    echo "No valid data collected across any benchmark."
fi

echo "All done."