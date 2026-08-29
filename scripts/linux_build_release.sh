#!/usr/bin/env bash
#
# Build a release binary of mdv on Linux.
#
# Environment overrides:
#   BUILD_DIR   Output build directory (default: <repo>/build-release)
#   CONFIG      CMake build type       (default: Release)
#   CMAKE_BIN   cmake executable       (default: selected Qt's bin/qt-cmake)
#   QT_ROOT     Qt 6.10.3+ prefix (default: Qt 6.11.2 under ~/Qt or /opt/Qt)
#   JOBS        Parallel build jobs    (default: all available cores)
#
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-"$ROOT_DIR/build-release"}"
CONFIG="${CONFIG:-Release}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"
IFS= read -r DEFAULT_QT_VERSION < "$ROOT_DIR/qt-default-version.txt"
[[ "$DEFAULT_QT_VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || {
  echo "error: invalid qt-default-version.txt: $DEFAULT_QT_VERSION" >&2
  exit 1
}

if [[ "$(uname -s)" != "Linux" ]]; then
  echo "error: this script is for Linux (detected $(uname -s))." >&2
  echo "       use scripts/macos_build_release.sh on macOS." >&2
  exit 1
fi

if [[ -z "${QT_ROOT:-}" ]]; then
  qt_candidates=()
  if [[ -n "${HOME:-}" ]]; then
    qt_candidates+=( "$HOME/Qt/$DEFAULT_QT_VERSION/gcc_64" )
  fi
  qt_candidates+=(
    "/opt/Qt/$DEFAULT_QT_VERSION/gcc_64"
    "/usr/local/Qt/$DEFAULT_QT_VERSION/gcc_64"
  )
  for qt_candidate in "${qt_candidates[@]}"; do
    if [[ -x "$qt_candidate/bin/qt-cmake" ]]; then
      QT_ROOT="$qt_candidate"
      break
    fi
  done
  if [[ -z "${QT_ROOT:-}" ]]; then
    echo "error: default Qt $DEFAULT_QT_VERSION was not found." >&2
    echo "       install it under ~/Qt or /opt/Qt, or set QT_ROOT explicitly." >&2
    exit 1
  fi
fi

if [[ ! -x "$QT_ROOT/bin/qt-cmake" ]]; then
  echo "error: QT_ROOT does not contain bin/qt-cmake: $QT_ROOT" >&2
  exit 1
fi
CMAKE_BIN="${CMAKE_BIN:-"$QT_ROOT/bin/qt-cmake"}"

cmake_args=(
  -S "$ROOT_DIR"
  -B "$BUILD_DIR"
  -DCMAKE_BUILD_TYPE="$CONFIG"
)
if [[ -n "${QT_ROOT:-}" ]]; then
  cmake_args+=( -DCMAKE_PREFIX_PATH="$QT_ROOT" )
fi

echo "==> Configuring ($CONFIG) with $CMAKE_BIN"
echo "==> Using Qt from $QT_ROOT"
"$CMAKE_BIN" "${cmake_args[@]}"

echo "==> Building with $JOBS job(s)"
cmake --build "$BUILD_DIR" --config "$CONFIG" --parallel "$JOBS"

echo "Built: $BUILD_DIR/mdv"
