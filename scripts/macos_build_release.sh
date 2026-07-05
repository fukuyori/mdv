#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-"$ROOT_DIR/build-release"}"
CONFIG="${CONFIG:-Release}"

if [[ -z "${CMAKE_BIN:-}" ]]; then
  if command -v qt-cmake >/dev/null 2>&1; then
    CMAKE_BIN="qt-cmake"
  else
    CMAKE_BIN="cmake"
  fi
fi

cmake_args=(
  -S "$ROOT_DIR"
  -B "$BUILD_DIR"
  -DCMAKE_BUILD_TYPE="$CONFIG"
  -DCMAKE_DISABLE_FIND_PACKAGE_WrapVulkanHeaders=ON
)

if [[ "$(uname -s)" == "Darwin" ]]; then
  cmake_args+=(
    -DCMAKE_OSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-26.0}"
  )
fi

"$CMAKE_BIN" "${cmake_args[@]}"
cmake --build "$BUILD_DIR" --config "$CONFIG" --parallel

if [[ "$(uname -s)" == "Darwin" ]]; then
  echo "Built: $BUILD_DIR/mdv.app"
else
  echo "Built: $BUILD_DIR/mdv"
fi
