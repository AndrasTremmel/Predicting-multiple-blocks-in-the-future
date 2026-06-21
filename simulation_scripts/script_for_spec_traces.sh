#!/bin/bash
#PBS -l select=1:ncpus=1:mem=16gb
#PBS -l walltime=72:00:00

cd $PBS_O_WORKDIR

CHAMPSIM_BIN="./champsim_for_IPC_analysis_for_baseline_with_limit_of_16_and_early_flush_penalty_of_2"
TRACE_DIR="./Traces"
SIMPOINT_ROOT="./Weights"

WARMUP=10000000
SIM=100000000

# Directory where every raw output file will live
RESULTS_DIR="$PBS_O_WORKDIR/IPC_analysis_for_baseline_with_limit_of_16_and_early_flush_penalty_of_2_results"
mkdir -p "$RESULTS_DIR"

echo "========================================"
echo "Starting ChampSim batch run"
echo "Results will be saved to: $RESULTS_DIR"
echo "========================================"

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

    exec 3< "$weight_file"

    while read -r sp_line && read -r weight <&3; do

        sp=$(echo "$sp_line" | awk '{print $1}')
        trace=$(ls "$TRACE_DIR"/${bench_name}-${sp}B.champsimtrace.xz 2>/dev/null)

        if [[ -z "$trace" ]]; then
            echo "Skipping simpoint $sp (no trace found)"
            continue
        fi

        echo "Running: $(basename "$trace") (weight=$weight)"

        # One file per (benchmark, simpoint) containing the FULL ChampSim output
        OUTFILE="${RESULTS_DIR}/${bench_name}_sp${sp}.txt"

        # Write header + full simulation output + footer
        {
            echo "===== BENCHMARK: $bench_name | SIMPOINT: $sp | WEIGHT: $weight ====="
            echo "===== BASELINE ====="
            echo ""
            $CHAMPSIM_BIN \
                --warmup-instructions $WARMUP \
                --simulation-instructions $SIM \
                "$trace"
            echo ""
            echo "===== END OF RUN ====="
        } > "$OUTFILE" 2>&1

    done < "$sim_file"

    exec 3<&-

    echo "Finished benchmark: $bench_name"

done

echo ""
echo "All benchmarks completed."
