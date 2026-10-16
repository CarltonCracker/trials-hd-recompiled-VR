#!/usr/bin/env bash
# add_dlc.sh - Add Trials HD DLC / Addon packages to the assets directory
#
# Usage:
#   ./add_dlc.sh <path_to_addon_folder_or_package> [assets_dir]
#
# Examples:
#   ./add_dlc.sh "game/"                                      # Scans and installs all DLC in game/
#   ./add_dlc.sh "game/Trials HD - Big Pack (World) (Addon)"  # Installs a single addon folder
#   ./add_dlc.sh "path/to/B1A493BA359C56825FB0BD67F37931CEFA71695958" # Installs a single package file
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PYTHON="${PYTHON:-python3}"

print_usage() {
  cat <<'EOF'
Usage:
  ./add_dlc.sh <path_to_addon_folder_or_package> [assets_dir]

Arguments:
  <path_to_addon_folder_or_package>   Path to an Xbox 360 STFS DLC package, an addon
                                      directory, or a directory containing multiple addons.
  [assets_dir]                        Target assets directory (default: assets/ in repository root).

Examples:
  ./add_dlc.sh "game/"
  ./add_dlc.sh "game/Trials HD - Big Pack (World) (Addon)"
  ./add_dlc.sh "game/Trials HD - Big Thrills (World) (Addon)"
EOF
}

if [[ "${1:-}" == "-h" ]] || [[ "${1:-}" == "--help" ]]; then
  print_usage
  exit 0
fi

if [[ $# -lt 1 ]]; then
  print_usage >&2
  exit 1
fi

INPUT_PATH="$1"
ASSETS_DIR="${2:-$ROOT_DIR/assets}"

if ! command -v "$PYTHON" >/dev/null 2>&1; then
  echo "Error: Python 3 ($PYTHON) is required but not found in PATH." >&2
  exit 1
fi

if [[ ! -e "$INPUT_PATH" ]]; then
  echo "Error: Specified path does not exist: $INPUT_PATH" >&2
  exit 1
fi

if [[ ! -d "$ASSETS_DIR" ]]; then
  echo "Error: Assets directory does not exist: $ASSETS_DIR" >&2
  echo "Please extract the base game assets first using ./build.sh --game <path>." >&2
  exit 1
fi

echo "============================================================"
echo " Trials HD Recompiled - Add DLC / Addons"
echo "============================================================"
echo "Source: $INPUT_PATH"
echo "Target: $ASSETS_DIR"
echo ""

"$PYTHON" "$ROOT_DIR/tools/add_dlc.py" "$INPUT_PATH" "$ASSETS_DIR"
echo ""
echo "DLC installation complete! The DLC tracks will be loaded when starting the game."
