#!/bin/bash
#PBS -l select=1:ncpus=1:mem=16gb
#PBS -l walltime=72:00:00

cd "$PBS_O_WORKDIR"

CHAMPSIM_BIN="./champsim_for_IPC_analysis_for_baseline_with_limit_of_16_and_early_flush_penalty_of_2"
TRACE_DIR="./Google_Traces"

WARMUP=10000000
SIM=100000000

# Directory where every raw output file will live
RESULTS_DIR="$PBS_O_WORKDIR/google_traces_IPC_analysis_for_baseline_with_limit_of_16_and_early_flush_penalty_of_2_results"
mkdir -p "$RESULTS_DIR"

# ============================================================
# ADJUST THIS if your Google traces use a different extension
TRACE_PATTERN="*.champsim.gz"
# ============================================================

echo "========================================"
echo "Starting ChampSim Google Traces batch run"
echo "Results will be saved to: $RESULTS_DIR"
echo "Trace pattern: $TRACE_PATTERN"
echo "========================================"

# Build list of traces recursively
TRACES=()
if [[ $# -gt 0 ]]; then
    # If arguments provided, treat them as subdirectory names to run
    for subdir in "$@"; do
        if [[ -d "$TRACE_DIR/$subdir" ]]; then
            while IFS= read -r -d '' f; do
                TRACES+=("$f")
            done < <(find "$TRACE_DIR/$subdir" -maxdepth 1 -type f -name "$TRACE_PATTERN" -print0)
        else
            echo "Warning: subdirectory '$subdir' not found in $TRACE_DIR"
        fi
    done
else
    # No arguments: run all traces in all subdirectories (maxdepth 2)
    while IFS= read -r -d '' f; do
        TRACES+=("$f")
    done < <(find "$TRACE_DIR" -maxdepth 2 -type f -name "$TRACE_PATTERN" -print0)
fi

if [[ ${#TRACES[@]} -eq 0 ]]; then
    echo "No traces found in $TRACE_DIR matching pattern: $TRACE_PATTERN"
    echo "Please check the trace files and adjust TRACE_PATTERN in the script."
    exit 1
fi

echo "Found ${#TRACES[@]} trace(s) to simulate."
echo ""

for trace_path in "${TRACES[@]}"; do
    trace_name=$(basename "$trace_path")
    trace_subdir=$(basename "$(dirname "$trace_path")")

    # Strip the extension for the output filename
    trace_base="${trace_name%.champsim.gz}"

    # Prefix with subdirectory name to avoid collisions across folders
    OUTFILE="${RESULTS_DIR}/${trace_subdir}_${trace_base}.txt"

    echo "----------------------------------------"
    echo "Running: $trace_subdir/$trace_name"
    echo "----------------------------------------"

    # Write header + full simulation output + footer
    {
        echo "===== TRACE: $trace_subdir/$trace_name ====="
        echo "===== MULTI_BLOCK ====="
        echo ""
        "$CHAMPSIM_BIN" \
            --warmup-instructions "$WARMUP" \
            --simulation-instructions "$SIM" \
            "$trace_path"
        echo ""
        echo "===== END OF RUN ====="
    } > "$OUTFILE" 2>&1

    echo "Finished: $trace_subdir/$trace_name"

done

echo ""
echo "All Google trace simulations completed."
echo "Results saved in: $RESULTS_DIR"
