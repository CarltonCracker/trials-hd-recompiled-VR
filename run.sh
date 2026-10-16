#!/usr/bin/env bash
#
# Launcher script for Trials HD Recompiled.
# Checks if the game executable is built and launches it with game assets.
#
# Usage:
#   ./run.sh [options] [extra game arguments]
#
# Options:
#   --release           Launch the Release build configuration
#   --debug             Launch the Debug build configuration
#   --relwithdebinfo    Launch the RelWithDebInfo build configuration (default)
#   --bin PATH          Path to a specific trials_hd executable
#   -h, --help          Show this help message
#
# Any additional arguments will be forwarded directly to trials_hd.
#
# Examples:
#   ./run.sh
#   ./run.sh --release
#   ./run.sh --gpu_plugin=xenos
#   ./run.sh --present_fps_limit=144
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"

# Detect OS & Architecture to find default preset
OS="$(uname -s)"
case "$OS" in
  Linux)
    PLATFORM="linux"
    ;;
  Darwin)
    PLATFORM="mac"
    ;;
  *)
    PLATFORM="linux"
    ;;
esac

ARCH="$(uname -m)"
case "$ARCH" in
  x86_64|amd64)
    ARCH_PRESET="amd64"
    ;;
  arm64|aarch64)
    ARCH_PRESET="arm64"
    ;;
  *)
    ARCH_PRESET="amd64"
    ;;
esac

CONFIG=""
EXE_PATH="${TRIALS_HD_EXE:-}"
GAME_ARGS=()

show_help() {
  cat <<EOF
Trials HD Recompiled - Run Script

Usage:
  ./run.sh [options] [extra game arguments]

Options:
  --release             Run the Release build configuration
  --debug               Run the Debug build configuration
  --relwithdebinfo      Run the RelWithDebInfo build configuration
  --bin PATH            Path to specific trials_hd binary
  -h, --help            Show this help message

Common game arguments:
  --gpu_plugin=webgpu   Use WebGPU renderer (default)
  --gpu_plugin=xenos    Use Vulkan renderer
  --present_fps_limit=N Target FPS cap (e.g. 60, 120, 144, 0 for uncapped)
  --host_vsync=bool     Enable or disable VSync (true/false)
  --log_level=level     Logging level (trace, debug, info, warn, err)

EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --release|release)
      CONFIG="release"
      shift
      ;;
    --debug|debug)
      CONFIG="debug"
      shift
      ;;
    --relwithdebinfo|relwithdebinfo)
      CONFIG="relwithdebinfo"
      shift
      ;;
    --bin)
      if [[ $# -lt 2 ]]; then
        echo "Error: --bin requires a path to an executable." >&2
        exit 1
      fi
      EXE_PATH="$2"
      shift 2
      ;;
    -h|--help)
      show_help
      exit 0
      ;;
    *)
      GAME_ARGS+=("$1")
      shift
      ;;
  esac
done

# If an explicit executable was not given, find it in the build directory
if [[ -z "$EXE_PATH" ]]; then
  if [[ -n "$CONFIG" ]]; then
    CANDIDATE="$ROOT_DIR/out/build/${PLATFORM}-${ARCH_PRESET}-${CONFIG}/trials_hd"
    if [[ -x "$CANDIDATE" ]]; then
      EXE_PATH="$CANDIDATE"
    fi
  else
    # Default search order: relwithdebinfo, release, debug, or any available
    for cfg in relwithdebinfo release debug; do
      CANDIDATE="$ROOT_DIR/out/build/${PLATFORM}-${ARCH_PRESET}-${cfg}/trials_hd"
      if [[ -x "$CANDIDATE" ]]; then
        EXE_PATH="$CANDIDATE"
        break
      fi
    done
    if [[ -z "$EXE_PATH" ]]; then
      # Check if any preset has trials_hd
      for found in "$ROOT_DIR"/out/build/*/trials_hd; do
        if [[ -x "$found" ]]; then
          EXE_PATH="$found"
          break
        fi
      done
    fi
  fi
fi

# Check if the game is built
if [[ -z "$EXE_PATH" || ! -x "$EXE_PATH" ]]; then
  echo "============================================================" >&2
  echo "Error: Trials HD executable not found or not built." >&2
  if [[ -n "$CONFIG" ]]; then
    echo "Checked: $ROOT_DIR/out/build/${PLATFORM}-${ARCH_PRESET}-${CONFIG}/trials_hd" >&2
  fi
  echo "" >&2
  echo "Please build the game first by running:" >&2
  echo "  ./build.sh" >&2
  echo "============================================================" >&2
  exit 1
fi

# If --game_data_root was not passed, pass repo assets if present
HAS_GAME_DATA_ROOT=0
if [[ ${#GAME_ARGS[@]} -gt 0 ]]; then
  for arg in "${GAME_ARGS[@]}"; do
    if [[ "$arg" == --game_data_root=* ]]; then
      HAS_GAME_DATA_ROOT=1
      break
    fi
  done
fi

if [[ $HAS_GAME_DATA_ROOT -eq 0 && -d "$ROOT_DIR/assets" ]]; then
  GAME_ARGS+=(--game_data_root="$ROOT_DIR/assets")
fi

echo "==> Launching Trials HD ($EXE_PATH)..."
exec "$EXE_PATH" "${GAME_ARGS[@]}"
