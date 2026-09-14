#!/usr/bin/env bash
set -uo pipefail

root=/tmp/xs-gem5-ideal-2-taken
output=/tmp/xs-gem5-ideal-2-taken-perf-cpython
workload_list=/home/xuejing/sim/workloads/cpython/workload_list/cpython-test.json
library_path=/home/xuejing/opt/miniforge3/lib

mkdir -p "$output"
export LD_LIBRARY_PATH="$library_path${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

python3 "$root/tests/gem5/ideal_2_taken/run_cpython_ab.py" \
    --gem5 "$root/build/RISCV/gem5.opt" \
    --config "$root/configs/example/kmhv3.py" \
    --workload-list "$workload_list" \
    --disable-difftest \
    --output-root "$output" \
    --jobs 8 \
    --maxinsts 50000000 \
    --library-path "$library_path" \
    >"$output/batch.log" 2>&1
runner_rc=$?
printf '%s\n' "$runner_rc" >"$output/batch.exit"

if [[ $runner_rc -eq 0 ]]; then
    python3 "$root/tests/gem5/ideal_2_taken/analyze_cpython_ab.py" \
        --workload-list "$workload_list" \
        --output-root "$output" \
        >"$output/analysis.log" 2>&1
    analysis_rc=$?
else
    analysis_rc=125
fi
printf '%s\n' "$analysis_rc" >"$output/analysis.exit"

if [[ $runner_rc -ne 0 ]]; then
    exit "$runner_rc"
fi
exit "$analysis_rc"
