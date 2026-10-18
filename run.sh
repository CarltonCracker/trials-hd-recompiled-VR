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
#   --web               Serve the browser build (./build.sh --web) on localhost
#   --port N            Web: port to serve on (default 8091)
#   --no-assets         Web: don't serve assets/; the page asks to import the game
#   -h, --help          Show this help message
#
# Any additional arguments will be forwarded directly to trials_hd (with --web,
# --name=value arguments become the page's runtime flags).
#
# Examples:
#   ./run.sh
#   ./run.sh --release
#   ./run.sh --gpu_plugin=xenos
#   ./run.sh --present_fps_limit=144
#   ./run.sh --web --log_guest_fps=true
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
WEB=0
WEB_PORT=8091
WEB_ASSETS=1

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
  --web                 Serve the browser build on http://127.0.0.1:PORT (open it in
                        Chrome/Edge/Chromium with WebGPU); Ctrl+C stops the server
  --port N              Web: port to serve on (default $WEB_PORT)
  --no-assets           Web: don't serve assets/; the page imports your game package
                        into browser storage instead
  -h, --help            Show this help message

Common game arguments:
  --gpu_plugin=xenos    Use Vulkan renderer (default)
  --gpu_plugin=webgpu   Use the WebGPU renderer (the web port's; for development)
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
    --web)
      WEB=1
      shift
      ;;
    --port)
      if [[ $# -lt 2 ]]; then
        echo "Error: --port requires a port number." >&2
        exit 1
      fi
      WEB_PORT="$2"
      shift 2
      ;;
    --no-assets)
      WEB_ASSETS=0
      shift
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

# -----------------------------------------------------------------------------
# Web build: check it is built, then serve it on localhost.
# -----------------------------------------------------------------------------
if [[ $WEB -eq 1 ]]; then
  WEB_DIR="$ROOT_DIR/out/build/web-wasm64-release/web"
  if [[ ! -f "$WEB_DIR/trials_hd.html" || ! -f "$WEB_DIR/trials_hd.wasm" || ! -f "$WEB_DIR/trials_hd.js" ]]; then
    echo "============================================================" >&2
    echo "Error: the web build was not found." >&2
    echo "Checked: $WEB_DIR/trials_hd.{html,js,wasm}" >&2
    echo "" >&2
    echo "Please build it first by running:" >&2
    echo "  ./build.sh --web" >&2
    echo "============================================================" >&2
    exit 1
  fi
  if ! command -v python3 >/dev/null 2>&1; then
    echo "Error: python3 is needed to serve the web build." >&2
    exit 1
  fi
  # Runtime flags travel in the page URL: --name=value -> name=value.
  QUERY=()
  SERVE_ARGS=("$WEB_DIR" --port "$WEB_PORT")
  if [[ $WEB_ASSETS -eq 1 && -f "$ROOT_DIR/assets/default.xex" ]]; then
    SERVE_ARGS+=(--assets "$ROOT_DIR/assets")
    QUERY+=("web_assets_url=/assets")
  fi
  if [[ ${#GAME_ARGS[@]} -gt 0 ]]; then
    for arg in "${GAME_ARGS[@]}"; do
      if [[ "$arg" != --* ]]; then
        echo "Error: with --web, game arguments must look like --name or --name=value (got '$arg')." >&2
        exit 1
      fi
      QUERY+=("${arg#--}")
    done
  fi
  URL="http://127.0.0.1:$WEB_PORT/trials_hd.html"
  if [[ ${#QUERY[@]} -gt 0 ]]; then
    URL+="?$(IFS='&'; echo "${QUERY[*]}")"
  fi
  echo "============================================================"
  echo " Trials HD (web) at:"
  echo "   $URL"
  echo " Open it in a WebGPU browser (Chrome/Edge/Chromium) on this machine;"
  if [[ " ${SERVE_ARGS[*]} " == *" --assets "* ]]; then
    echo " game data is served from assets/ (use --no-assets to test the import)."
  else
    echo " the page asks for your game package or folder on first start."
  fi
  echo " Local use only: never publish this build. Ctrl+C stops the server."
  echo "============================================================"
  exec python3 "$ROOT_DIR/tools/web/serve.py" "${SERVE_ARGS[@]}"
fi

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
