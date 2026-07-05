#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SVG_PATH="${SVG_PATH:-"$ROOT_DIR/resources/icon.svg"}"
ICONSET_DIR="$ROOT_DIR/resources/macos/mdv.iconset"
ICNS_PATH="$ROOT_DIR/resources/macos/mdv.icns"
RENDERER_BUILD_DIR="${RENDERER_BUILD_DIR:-"$ROOT_DIR/build-icon-renderer"}"
RENDERER="$RENDERER_BUILD_DIR/mdv_icon_renderer"

mkdir -p "$ICONSET_DIR"

if [[ ! -x "$RENDERER" ]]; then
  if command -v qt-cmake >/dev/null 2>&1; then
    qt-cmake -S "$ROOT_DIR/tools/icon_renderer" -B "$RENDERER_BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
  else
    cmake -S "$ROOT_DIR/tools/icon_renderer" -B "$RENDERER_BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
  fi
  cmake --build "$RENDERER_BUILD_DIR" --config Release --parallel
fi

render_png() {
  local size="$1"
  local output="$2"
  "$RENDERER" "$SVG_PATH" "$size" "$output"
}

render_png 16 "$ICONSET_DIR/icon_16x16.png"
render_png 32 "$ICONSET_DIR/icon_16x16@2x.png"
render_png 32 "$ICONSET_DIR/icon_32x32.png"
render_png 64 "$ICONSET_DIR/icon_32x32@2x.png"
render_png 128 "$ICONSET_DIR/icon_128x128.png"
render_png 256 "$ICONSET_DIR/icon_128x128@2x.png"
render_png 256 "$ICONSET_DIR/icon_256x256.png"
render_png 512 "$ICONSET_DIR/icon_256x256@2x.png"
render_png 512 "$ICONSET_DIR/icon_512x512.png"
render_png 1024 "$ICONSET_DIR/icon_512x512@2x.png"

iconutil -c icns "$ICONSET_DIR" -o "$ICNS_PATH"
echo "Created: $ICNS_PATH"
