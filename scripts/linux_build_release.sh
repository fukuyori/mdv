#!/usr/bin/env bash
#
# Build a release binary of mdv on Linux.
#
# Environment overrides:
#   BUILD_DIR   Output build directory (default: <repo>/build-release)
#   CONFIG      CMake build type       (default: Release)
#   CMAKE_BIN   cmake executable       (default: qt-cmake if present, else cmake)
#   JOBS        Parallel build jobs    (default: all available cores)
#
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-"$ROOT_DIR/build-release"}"
CONFIG="${CONFIG:-Release}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"

if [[ "$(uname -s)" != "Linux" ]]; then
  echo "error: this script is for Linux (detected $(uname -s))." >&2
  echo "       use scripts/macos_build_release.sh on macOS." >&2
  exit 1
fi

if [[ -z "${CMAKE_BIN:-}" ]]; then
  if command -v qt-cmake >/dev/null 2>&1; then
    CMAKE_BIN="qt-cmake"
  else
    CMAKE_BIN="cmake"
  fi
fi

echo "==> Configuring ($CONFIG) with $CMAKE_BIN"
"$CMAKE_BIN" \
  -S "$ROOT_DIR" \
  -B "$BUILD_DIR" \
  -DCMAKE_BUILD_TYPE="$CONFIG"

echo "==> Building with $JOBS job(s)"
cmake --build "$BUILD_DIR" --config "$CONFIG" --parallel "$JOBS"

echo "Built: $BUILD_DIR/mdv"
