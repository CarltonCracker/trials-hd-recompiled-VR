#!/usr/bin/env bash
# Build script for Trials HD Recompiled (desktop Linux and macOS).
#
# Automatically detects host operating system and CPU architecture,
# verifies or extracts game assets from an STFS package, configures CMake
# using the corresponding preset, and builds the project.
# Automatically handles the initial two-pass codegen build on fresh checkouts.
#
# Usage:
#   ./build.sh [options] [extra cmake/build args]
#
# Options:
#   --game, -g PATH     Path to game folder or STFS package (e.g. "Trials HD (XBLA)")
#                       Required when the assets/ directory is missing or incomplete.
#   --force-extract     Re-extract game assets even if assets/ is already verified
#   --release           Build Release configuration
#   --debug             Build Debug configuration
#   --relwithdebinfo    Build RelWithDebInfo configuration (default)
#   -j, --jobs N        Number of parallel compile jobs (default: CPU core count)
#   --clean             Remove the preset's build directory before building
#   --clean-all         Remove the entire out/build directory before building
#   --no-webgpu         Build with Vulkan renderer only (disables WebGPU/Dawn)
#   --target TARGET     Build a specific CMake target (default: trials_hd)
#   -h, --help          Show this help message
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"

# -----------------------------------------------------------------------------
# OS & Architecture Detection
# -----------------------------------------------------------------------------
OS="$(uname -s)"
case "$OS" in
  Linux)
    PLATFORM="linux"
    DEFAULT_JOBS="$(nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
    ;;
  Darwin)
    PLATFORM="mac"
    DEFAULT_JOBS="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"
    ;;
  *)
    echo "Error: Unsupported operating system '$OS'." >&2
    echo "This script supports desktop Linux and macOS only." >&2
    exit 1
    ;;
esac

ARCH="$(uname -m)"
case "$ARCH" in
  x86_64|amd64)
    ARCH_PRESET="amd64"
    ;;
  arm64|aarch64)
    if [[ "$PLATFORM" == "linux" ]]; then
      echo "Error: Linux ARM64 is not currently supported." >&2
      echo "Only x86-64 Linux (SSE4.2 / x86-64-v2) is supported." >&2
      exit 1
    fi
    ARCH_PRESET="arm64"
    ;;
  *)
    echo "Error: Unsupported CPU architecture '$ARCH'." >&2
    exit 1
    ;;
esac

# -----------------------------------------------------------------------------
# Command-line Options
# -----------------------------------------------------------------------------
CONFIG="relwithdebinfo"
JOBS="$DEFAULT_JOBS"
GAME_DIR=""
FORCE_EXTRACT=0
DO_CLEAN=0
DO_CLEAN_ALL=0
TARGET="trials_hd"
CMAKE_EXTRA_ARGS=()
BUILD_EXTRA_ARGS=()

show_help() {
  cat <<EOF
Trials HD Recompiled - Build Script

Usage:
  ./build.sh [options] [extra cmake/build args]

Platform detected: $PLATFORM ($ARCH)

Options:
  --game, -g PATH       Path to game folder or STFS package (e.g. "Trials HD (XBLA)")
                        Required if the assets/ folder is missing or incomplete.
  --force-extract       Force re-extraction of assets even if assets/ already exists
  --release             Build Release configuration
  --debug               Build Debug configuration
  --relwithdebinfo      Build RelWithDebInfo configuration (default)
  -j, --jobs N          Compile with N parallel jobs (default: $DEFAULT_JOBS)
  --clean               Clean the current preset build directory
  --clean-all           Delete out/build entirely
  --no-webgpu           Disable WebGPU renderer (build Vulkan only)
  --target TARGET       Build a specific CMake target (default: trials_hd)
  -h, --help            Show this help message

Examples:
  ./build.sh                                          # Build if assets/ is already present
  ./build.sh --game "Trials HD (XBLA)"                # Extract assets (if missing) and build
  ./build.sh --release -j8                            # Optimized release build with 8 jobs
  ./build.sh --clean                                  # Fresh rebuild
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --game|--game-dir|--game-path|-g)
      if [[ $# -lt 2 ]]; then
        echo "Error: $1 requires a path to the game folder." >&2
        exit 1
      fi
      GAME_DIR="$2"
      shift 2
      ;;
    --force-extract)
      FORCE_EXTRACT=1
      shift
      ;;
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
    -j)
      if [[ $# -lt 2 ]]; then
        echo "Error: -j requires a number of jobs." >&2
        exit 1
      fi
      JOBS="$2"
      shift 2
      ;;
    -j*)
      JOBS="${1#-j}"
      shift
      ;;
    --jobs)
      if [[ $# -lt 2 ]]; then
        echo "Error: --jobs requires an argument." >&2
        exit 1
      fi
      JOBS="$2"
      shift 2
      ;;
    --clean)
      DO_CLEAN=1
      shift
      ;;
    --clean-all)
      DO_CLEAN_ALL=1
      shift
      ;;
    --no-webgpu|--vulkan-only)
      CMAKE_EXTRA_ARGS+=("-DTRIALSHD_WEBGPU=OFF")
      shift
      ;;
    --target|-t)
      if [[ $# -lt 2 ]]; then
        echo "Error: --target requires a target name." >&2
        exit 1
      fi
      TARGET="$2"
      shift 2
      ;;
    -h|--help)
      show_help
      exit 0
      ;;
    -D*)
      CMAKE_EXTRA_ARGS+=("$1")
      shift
      ;;
    *)
      # Allow positional argument as game path if it exists on disk and GAME_DIR not set
      if [[ -z "$GAME_DIR" && (-d "$1" || -f "$1") ]]; then
        GAME_DIR="$1"
      else
        BUILD_EXTRA_ARGS+=("$1")
      fi
      shift
      ;;
  esac
done

PRESET="${PLATFORM}-${ARCH_PRESET}-${CONFIG}"
BUILD_DIR="$ROOT_DIR/out/build/$PRESET"
ASSETS_DIR="$ROOT_DIR/assets"

# -----------------------------------------------------------------------------
# Asset Verification & Extraction
# -----------------------------------------------------------------------------
verify_assets() {
  local dir="$1"
  [[ -d "$dir" ]] && \
  [[ -f "$dir/default.xex" && -s "$dir/default.xex" ]] && \
  [[ -f "$dir/data/data.pak" && -s "$dir/data/data.pak" ]] && \
  [[ -f "$dir/data/data_menu.pak" && -s "$dir/data/data_menu.pak" ]] && \
  [[ -f "$dir/package_license.toml" && -s "$dir/package_license.toml" ]]
}

locate_package() {
  local search_path="$1"
  python3 - "$search_path" <<'PYEOF'
import os, sys, struct

target = sys.argv[1]
if not os.path.exists(target):
    sys.exit(1)

if os.path.isfile(target):
    try:
        with open(target, 'rb') as f:
            if f.read(4) in (b'LIVE', b'PIRS', b'CON '):
                print(os.path.abspath(target))
                sys.exit(0)
    except Exception:
        pass
    sys.exit(1)

candidates = []
for root, dirs, files in os.walk(target):
    for f in files:
        p = os.path.join(root, f)
        try:
            if os.path.getsize(p) < 0x400:
                continue
            with open(p, 'rb') as fp:
                magic = fp.read(4)
                if magic in (b'LIVE', b'PIRS', b'CON '):
                    fp.seek(0x344)
                    ctype = struct.unpack('>I', fp.read(4))[0]
                    fp.seek(0x360)
                    tid = struct.unpack('>I', fp.read(4))[0]
                    score = 0
                    if tid == 0x5841095A:
                        score += 10
                    if ctype == 0x000D0000:
                        score += 20
                    candidates.append((score, os.path.abspath(p)))
        except Exception:
            pass

if candidates:
    candidates.sort(key=lambda x: x[0], reverse=True)
    print(candidates[0][1])
    sys.exit(0)

sys.exit(1)
PYEOF
}

if verify_assets "$ASSETS_DIR" && [[ $FORCE_EXTRACT -eq 0 ]]; then
  echo "==> Assets verified in assets/ (all required files present; skipping extraction)"
else
  if [[ -z "$GAME_DIR" ]]; then
    echo "============================================================" >&2
    echo "Error: The 'assets' directory is missing or incomplete." >&2
    echo "Trials HD requires game assets (default.xex, data.pak, package_license.toml)." >&2
    echo "" >&2
    echo "Please provide the path to your game folder using --game:" >&2
    echo "  ./build.sh --game \"Trials HD (XBLA)\"" >&2
    echo "============================================================" >&2
    exit 1
  fi

  if [[ ! -e "$GAME_DIR" ]]; then
    echo "Error: Specified game path does not exist: $GAME_DIR" >&2
    exit 1
  fi

  echo "==> Locating Xbox 360 STFS game package in '$GAME_DIR'..."
  if ! PACKAGE_FILE="$(locate_package "$GAME_DIR")"; then
    echo "Error: Could not find a valid Xbox 360 STFS game package in '$GAME_DIR'." >&2
    echo "Expected an STFS package (LIVE/PIRS/CON) for Trials HD (title 5841095A)." >&2
    exit 1
  fi

  echo "==> Found package: $PACKAGE_FILE"
  echo "==> Extracting to $ASSETS_DIR..."
  mkdir -p "$ASSETS_DIR"
  python3 "$ROOT_DIR/tools/stfs_extract.py" "$PACKAGE_FILE" "$ASSETS_DIR"

  if ! verify_assets "$ASSETS_DIR"; then
    echo "Error: Asset extraction completed but required files were not found in $ASSETS_DIR." >&2
    exit 1
  fi
  echo "==> Assets extracted and verified successfully."
fi

# -----------------------------------------------------------------------------
# Tool & Environment Checks
# -----------------------------------------------------------------------------
# Prefer repo's venv ninja if system has none
if ! command -v ninja >/dev/null 2>&1 && [[ -x "$ROOT_DIR/.venv/bin/ninja" ]]; then
  export PATH="$ROOT_DIR/.venv/bin:$PATH"
fi

if ! command -v cmake >/dev/null 2>&1; then
  echo "Error: cmake was not found in PATH. Please install CMake 3.25+." >&2
  exit 1
fi

if ! command -v ninja >/dev/null 2>&1; then
  echo "Error: ninja was not found in PATH. Please install ninja-build." >&2
  exit 1
fi

if ! command -v clang++ >/dev/null 2>&1; then
  echo "Warning: clang++ not found in PATH. CMake preset expects Clang." >&2
fi

# Submodule initialization check
if [[ ! -f "$ROOT_DIR/thirdparty/rexglue-sdk/CMakeLists.txt" ]]; then
  echo "==> ReXGlue SDK submodule not found. Initializing git submodules..."
  git -C "$ROOT_DIR" submodule update --init --recursive --depth 1
fi

# -----------------------------------------------------------------------------
# Clean handling
# -----------------------------------------------------------------------------
if [[ $DO_CLEAN_ALL -eq 1 ]]; then
  echo "==> Removing out/build..."
  rm -rf "$ROOT_DIR/out/build"
elif [[ $DO_CLEAN -eq 1 ]]; then
  echo "==> Cleaning build directory: $BUILD_DIR"
  rm -rf "$BUILD_DIR"
fi

# -----------------------------------------------------------------------------
# Configure & Build
# -----------------------------------------------------------------------------
echo "============================================================"
echo " Building Trials HD Recompiled"
echo " OS:        $PLATFORM ($ARCH)"
echo " Preset:    $PRESET"
echo " Build dir: $BUILD_DIR"
echo " Target:    $TARGET"
echo " Jobs:      $JOBS"
echo "============================================================"

# Configure preset if not yet configured, or when CMake flags were supplied, or when cleaning
if [[ ! -f "$BUILD_DIR/build.ninja" ]] || [[ ${#CMAKE_EXTRA_ARGS[@]} -gt 0 ]]; then
  echo "==> Configuring with preset '$PRESET'..."
  cmake -S "$ROOT_DIR" --preset "$PRESET" "${CMAKE_EXTRA_ARGS[@]}"
fi

BUILD_CMD=(cmake --build "$BUILD_DIR" -j"$JOBS")
if [[ -n "$TARGET" ]]; then
  BUILD_CMD+=(--target "$TARGET")
fi
if [[ ${#BUILD_EXTRA_ARGS[@]} -gt 0 ]]; then
  BUILD_CMD+=("${BUILD_EXTRA_ARGS[@]}")
fi

echo "==> Compiling..."
"${BUILD_CMD[@]}"

# Handle first-time two-pass codegen requirement
# When codegen runs for the first time, trials_hd is excluded from ALL until the
# generated sources are written and CMake is re-run.
if [[ "$TARGET" == "trials_hd" ]] && [[ ! -f "$BUILD_DIR/trials_hd" ]]; then
  if [[ -f "$BUILD_DIR/trials_hd_codegen_ready.stamp" ]]; then
    echo "==> Initial codegen completed. Running build pass to compile generated sources..."
    "${BUILD_CMD[@]}"
  fi
fi

if [[ -f "$BUILD_DIR/trials_hd" ]]; then
  echo "============================================================"
  echo " Build complete!"
  echo " Executable: $BUILD_DIR/trials_hd"
  echo " Run with:   $BUILD_DIR/trials_hd --game_data_root=$ROOT_DIR/assets"
  echo "============================================================"
fi
