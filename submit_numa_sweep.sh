#!/usr/bin/env bash
set -euo pipefail

# 你可以在这里修改默认参数，也可在命令行通过环境变量覆盖：
# SIZE_MB=2048 ITERS=80 STRIDE=4096 BW_THREADS=8 ./submit_numa_sweep.sh

OUTDIR="${OUTDIR:-$PWD/numa_out_run_$(date +%Y%m%d_%H%M%S)}"
mkdir -p "$OUTDIR"

echo "Submitting Slurm job..."
jid=$(sbatch \
  --export=ALL,OUTDIR="$OUTDIR",SIZE_MB="${SIZE_MB:-1024}",ITERS="${ITERS:-50}",STRIDE="${STRIDE:-4096}",BW_THREADS="${BW_THREADS:-4}" \
  "$PWD/numa_sweep.sbatch" | awk '{print $4}')

echo "Submitted jobid=$jid"
echo "Outputs will be in: $OUTDIR"
echo "Check: slurm-${jid}.out / slurm-${jid}.err (in submit dir unless overridden by Slurm config)"
