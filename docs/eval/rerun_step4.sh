#!/bin/bash
# Step-4 full re-run on the final e-graph code (typed DSL + B-full + guarded
# expansion). Only EAN-config raw was invalidated; non-EAN cache is reused.
set -u
cd /d/Code/lotus/lotus/docs/eval
export EAN_WORKERS=8
ts() { date '+%H:%M:%S'; }

echo "[$(ts)] ===== real_eval run (all suites) ====="
python real_eval.py run 2>&1 | tail -3
echo "[$(ts)] ===== real_eval agg ====="
python real_eval.py agg 2>&1 | tail -3

echo "[$(ts)] ===== translapa run (coreutils) ====="
python translapa_eval.py run --suite coreutils --workers 8 2>&1 | tail -3
echo "[$(ts)] ===== translapa agg (coreutils) ====="
python translapa_eval.py agg --suite coreutils 2>&1 | tail -4

echo "[$(ts)] ===== affine run (all suites) ====="
python affine_eval.py run --workers 8 2>&1 | tail -3
echo "[$(ts)] ===== affine agg ====="
python affine_eval.py agg 2>&1 | tail -3

echo "[$(ts)] ===== ALL DONE ====="
