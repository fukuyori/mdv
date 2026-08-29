#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-"$ROOT_DIR/build-release"}"
CONFIG="${CONFIG:-Release}"
IFS= read -r DEFAULT_QT_VERSION < "$ROOT_DIR/qt-default-version.txt"
[[ "$DEFAULT_QT_VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || {
  echo "error: invalid qt-default-version.txt: $DEFAULT_QT_VERSION" >&2
  exit 1
}

if [[ -z "${QT_ROOT:-}" ]]; then
  qt_candidates=()
  if [[ -n "${HOME:-}" ]]; then
    qt_candidates+=( "$HOME/Qt/$DEFAULT_QT_VERSION/macos" )
  fi
  qt_candidates+=(
    "/opt/Qt/$DEFAULT_QT_VERSION/macos"
    "/usr/local/Qt/$DEFAULT_QT_VERSION/macos"
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
  -DCMAKE_DISABLE_FIND_PACKAGE_WrapVulkanHeaders=ON
)
if [[ -n "${QT_ROOT:-}" ]]; then
  cmake_args+=( -DCMAKE_PREFIX_PATH="$QT_ROOT" )
fi

if [[ "$(uname -s)" == "Darwin" ]]; then
  cmake_args+=(
    -DCMAKE_OSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-26.0}"
  )
fi

echo "Using Qt from $QT_ROOT"
"$CMAKE_BIN" "${cmake_args[@]}"
cmake --build "$BUILD_DIR" --config "$CONFIG" --parallel

if [[ "$(uname -s)" == "Darwin" ]]; then
  echo "Built: $BUILD_DIR/mdv.app"
else
  echo "Built: $BUILD_DIR/mdv"
fi
