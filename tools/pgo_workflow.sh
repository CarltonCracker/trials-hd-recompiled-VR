#!/usr/bin/env bash
# Automated Profile-Guided Optimization (PGO) Workflow for Trials HD Recompiled
#
# Usage:
#   tools/pgo_workflow.sh [command] [args...]
#
# Commands:
#   generate          Build the PGO instrumentation binary (linux-amd64-pgo-generate)
#   train             Run headless game ride workload to gather runtime profiles
#   merge             Merge gathered .profraw files into out/pgo_profiles/trials_hd.profdata
#   use               Build the PGO-optimized binary (linux-amd64-pgo-use)
#   verify            Run test suite and headless ride benchmark check on pgo-use binary
#   run-all           Run all steps: generate -> train -> merge -> use -> verify
#
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
PGO_DIR="$ROOT_DIR/out/pgo_profiles"
PROFDATA="$PGO_DIR/trials_hd.profdata"
GEN_PRESET="linux-amd64-pgo-generate"
USE_PRESET="linux-amd64-pgo-use"
GEN_BUILD_DIR="$ROOT_DIR/out/build/$GEN_PRESET"
USE_BUILD_DIR="$ROOT_DIR/out/build/$USE_PRESET"
LLVM_PROFDATA="${LLVM_PROFDATA:-$(which llvm-profdata 2>/dev/null || echo /usr/lib64/rocm/llvm/bin/llvm-profdata)}"
LOCK_FILE="$(git -C "$ROOT_DIR" rev-parse --path-format=absolute --git-common-dir 2>/dev/null || echo "$ROOT_DIR/.git")/vk-bench.lock"

mkdir -p "$PGO_DIR"

with_lock() {
  flock "$LOCK_FILE" "$@"
}

cmd_generate() {
  echo "=== Step: Building PGO Instrumentation Binary ($GEN_PRESET) ==="
  with_lock cmake --preset "$GEN_PRESET"
  with_lock cmake --build "$GEN_BUILD_DIR" -j"$(nproc 2>/dev/null || echo 4)"
  echo "PGO instrumentation build successful: $GEN_BUILD_DIR/trials_hd"
}

cmd_train() {
  echo "=== Step: Running Training Workload on Ride Benchmark ==="
  rm -f "$PGO_DIR"/*.profraw
  export LLVM_PROFILE_FILE="$PGO_DIR/trials_hd_%p_%m.profraw"
  export EXE="$GEN_BUILD_DIR/trials_hd"
  "$ROOT_DIR/tools/bench_vulkan.sh" "$PGO_DIR/train_run" 1280 720 --draw_resolution_scale_x=1 --draw_resolution_scale_y=1
  local raw_count
  raw_count=$(find "$PGO_DIR" -name "*.profraw" | wc -l)
  echo "Gathered $raw_count profile raw file(s) in $PGO_DIR"
  if [[ "$raw_count" -eq 0 ]]; then
    echo "Error: No .profraw files were generated during training workload." >&2
    exit 1
  fi
}

cmd_merge() {
  echo "=== Step: Merging Profiles with llvm-profdata ==="
  if ! command -v "$LLVM_PROFDATA" &>/dev/null && [[ ! -x "$LLVM_PROFDATA" ]]; then
    echo "Error: llvm-profdata not found at '$LLVM_PROFDATA'." >&2
    exit 1
  fi
  local raw_files
  raw_files=$(find "$PGO_DIR" -name "*.profraw")
  if [[ -z "$raw_files" ]]; then
    echo "Warning: No .profraw files found in $PGO_DIR to merge."
    return 0
  fi
  "$LLVM_PROFDATA" merge -output="$PROFDATA" "$PGO_DIR"/*.profraw
  echo "Successfully generated indexed profile: $PROFDATA ($(stat -c%s "$PROFDATA") bytes)"
}

cmd_use() {
  echo "=== Step: Building PGO-Optimized Binary ($USE_PRESET) ==="
  if [[ ! -s "$PROFDATA" ]]; then
    echo "Error: Profile data '$PROFDATA' does not exist or is empty. Run 'train' and 'merge' first." >&2
    exit 1
  fi
  with_lock cmake --preset "$USE_PRESET"
  with_lock cmake --build "$USE_BUILD_DIR" -j"$(nproc 2>/dev/null || echo 4)"
  echo "PGO-optimized build successful: $USE_BUILD_DIR/trials_hd"
}

cmd_verify() {
  echo "=== Step: Verifying PGO-Optimized Build ==="
  echo "Running unit & regression tests..."
  ctest --test-dir "$USE_BUILD_DIR" --output-on-failure
  echo "Running headless ride verification check..."
  export EXE="$USE_BUILD_DIR/trials_hd"
  "$ROOT_DIR/tools/bench_vulkan.sh" "$PGO_DIR/verify_run" 1280 720 --draw_resolution_scale_x=1 --draw_resolution_scale_y=1
  python3 "$ROOT_DIR/tests/check_fps_log.py" "$PGO_DIR/verify_run/trials_hd.log"
  echo "Verification complete: PGO-optimized binary passes all checks!"
}

cmd_all() {
  cmd_generate
  cmd_train
  cmd_merge
  cmd_use
  cmd_verify
}

COMMAND="${1:-all}"
shift || true

case "$COMMAND" in
  generate|build-generate)
    cmd_generate
    ;;
  train)
    cmd_train
    ;;
  merge)
    cmd_merge
    ;;
  use|build-use)
    cmd_use
    ;;
  verify)
    cmd_verify
    ;;
  all|run-all)
    cmd_all
    ;;
  *)
    echo "Unknown command: $COMMAND" >&2
    echo "Usage: $0 {generate|train|merge|use|verify|run-all}" >&2
    exit 1
    ;;
esac
