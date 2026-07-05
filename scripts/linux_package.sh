#!/usr/bin/env bash
#
# Create Linux install packages for mdv.
#
# Formats:
#   deb        Debian/Ubuntu .deb package (depends on the system Qt packages)
#   tarball    Portable .tar.gz with the binary + an install.sh/uninstall.sh
#   appimage   Self-contained .AppImage (bundles Qt; needs linuxdeploy)
#   all        Build every format above
#
# Usage:
#   scripts/linux_package.sh [FORMAT ...]      # default: deb tarball
#   scripts/linux_package.sh all
#
# Environment overrides:
#   BUILD_DIR   Release build directory (default: <repo>/build-release)
#   DIST_DIR    Output directory        (default: <repo>/dist)
#   MAINTAINER  .deb Maintainer field   (default: fukuyori <fukuyori.n@gmail.com>)
#   NO_BUILD=1  Skip building even if the binary is missing (fail instead)
#
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-"$ROOT_DIR/build-release"}"
DIST_DIR="${DIST_DIR:-"$ROOT_DIR/dist"}"
MAINTAINER="${MAINTAINER:-fukuyori <fukuyori.n@gmail.com>}"

BINARY="$BUILD_DIR/mdv"
DESKTOP_FILE="$ROOT_DIR/resources/linux/mdv.desktop"
ICON_FILE="$ROOT_DIR/resources/icon.svg"
METAINFO_FILE="$ROOT_DIR/resources/linux/com.fukuyori.mdv.metainfo.xml"

log()  { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33mwarning:\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

[[ "$(uname -s)" == "Linux" ]] || die "this script only runs on Linux (detected $(uname -s))."

VERSION="$(sed -n 's/^project(.*VERSION \([0-9][0-9.]*\).*/\1/p' "$ROOT_DIR/CMakeLists.txt" | head -n1)"
[[ -n "$VERSION" ]] || die "could not read version from CMakeLists.txt"

# -------------------------------------------------------------------------
# Ensure a release binary exists.
# -------------------------------------------------------------------------
ensure_binary() {
  if [[ -x "$BINARY" ]]; then
    return
  fi
  if [[ "${NO_BUILD:-0}" == "1" ]]; then
    die "binary not found at $BINARY (NO_BUILD=1 set)."
  fi
  log "Binary not found; running scripts/linux_build_release.sh"
  BUILD_DIR="$BUILD_DIR" "$ROOT_DIR/scripts/linux_build_release.sh"
  [[ -x "$BINARY" ]] || die "build finished but $BINARY is missing."
}

# -------------------------------------------------------------------------
# Shared: populate a `usr/` tree under the given prefix directory.
# -------------------------------------------------------------------------
stage_tree() {
  local dest="$1"          # e.g. .../pkgroot  -> creates $dest/usr/...
  install -Dm755 "$BINARY"        "$dest/usr/bin/mdv"
  install -Dm644 "$DESKTOP_FILE"  "$dest/usr/share/applications/mdv.desktop"
  install -Dm644 "$ICON_FILE"     "$dest/usr/share/icons/hicolor/scalable/apps/mdv.svg"
  if [[ -f "$METAINFO_FILE" ]]; then
    install -Dm644 "$METAINFO_FILE" "$dest/usr/share/metainfo/com.fukuyori.mdv.metainfo.xml"
  fi
}

# -------------------------------------------------------------------------
# .deb package
# -------------------------------------------------------------------------
build_deb() {
  command -v dpkg-deb >/dev/null 2>&1 || { warn "dpkg-deb not found; skipping deb."; return; }

  local arch pkgroot depends
  arch="$(dpkg --print-architecture)"
  pkgroot="$DIST_DIR/deb/mdv_${VERSION}_${arch}"
  rm -rf "$pkgroot"

  log "Building .deb ($arch)"
  stage_tree "$pkgroot"

  # Resolve shared-library dependencies from the binary when possible,
  # otherwise fall back to a sensible Qt6 WebEngine dependency set.
  # dpkg-shlibdeps needs a lowercase debian/control with a Source: field in the
  # working directory; we create a throwaway one and remove it before packing.
  depends=""
  if command -v dpkg-shlibdeps >/dev/null 2>&1; then
    mkdir -p "$pkgroot/debian"
    printf 'Source: mdv\n' > "$pkgroot/debian/control"
    if ( cd "$pkgroot" && dpkg-shlibdeps -O "usr/bin/mdv" 2>/dev/null ) > "$pkgroot/.shlibdeps" ; then
      depends="$(sed -n 's/^shlibs:Depends=//p' "$pkgroot/.shlibdeps")"
    fi
    rm -rf "$pkgroot/debian" "$pkgroot/.shlibdeps"
  fi
  if [[ -z "$depends" ]]; then
    warn "falling back to a default Qt6 dependency list for the .deb."
    depends="libqt6widgets6, libqt6webenginewidgets6, libqt6webenginecore6, libqt6core6, libqt6gui6"
  fi

  mkdir -p "$pkgroot/DEBIAN"
  cat > "$pkgroot/DEBIAN/control" <<EOF
Package: mdv
Version: $VERSION
Section: editors
Priority: optional
Architecture: $arch
Maintainer: $MAINTAINER
Depends: $depends
Description: Simple Markdown viewer/editor
 mdv is a lightweight Markdown viewer and editor with a live WebEngine
 preview, synchronized scrolling, and find/replace.
EOF

  # Post-install / post-removal hooks to refresh desktop & icon caches.
  cat > "$pkgroot/DEBIAN/postinst" <<'EOF'
#!/bin/sh
set -e
if command -v update-desktop-database >/dev/null 2>&1; then
  update-desktop-database -q /usr/share/applications || true
fi
if command -v gtk-update-icon-cache >/dev/null 2>&1; then
  gtk-update-icon-cache -q -t -f /usr/share/icons/hicolor || true
fi
EOF
  cp "$pkgroot/DEBIAN/postinst" "$pkgroot/DEBIAN/postrm"
  chmod 755 "$pkgroot/DEBIAN/postinst" "$pkgroot/DEBIAN/postrm"

  local out="$DIST_DIR/mdv-${VERSION}-linux-${arch}.deb"
  dpkg-deb --build --root-owner-group "$pkgroot" "$out"
  log "Created $out"
}

# -------------------------------------------------------------------------
# Portable tarball (relies on system Qt) with install/uninstall scripts.
# -------------------------------------------------------------------------
build_tarball() {
  local arch stage name
  arch="$(uname -m)"
  name="mdv-${VERSION}-linux-${arch}"
  stage="$DIST_DIR/tarball/$name"
  rm -rf "$stage"

  log "Building portable tarball ($arch)"
  install -Dm755 "$BINARY"        "$stage/bin/mdv"
  install -Dm644 "$DESKTOP_FILE"  "$stage/share/applications/mdv.desktop"
  install -Dm644 "$ICON_FILE"     "$stage/share/icons/hicolor/scalable/apps/mdv.svg"
  [[ -f "$METAINFO_FILE" ]] && install -Dm644 "$METAINFO_FILE" \
    "$stage/share/metainfo/com.fukuyori.mdv.metainfo.xml"

  cat > "$stage/install.sh" <<'EOF'
#!/usr/bin/env bash
# Install mdv into a prefix (default: ~/.local; use PREFIX=/usr/local for system-wide).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PREFIX="${PREFIX:-$HOME/.local}"
install -Dm755 "$HERE/bin/mdv"                     "$PREFIX/bin/mdv"
install -Dm644 "$HERE/share/applications/mdv.desktop" "$PREFIX/share/applications/mdv.desktop"
install -Dm644 "$HERE/share/icons/hicolor/scalable/apps/mdv.svg" \
               "$PREFIX/share/icons/hicolor/scalable/apps/mdv.svg"
if [ -f "$HERE/share/metainfo/com.fukuyori.mdv.metainfo.xml" ]; then
  install -Dm644 "$HERE/share/metainfo/com.fukuyori.mdv.metainfo.xml" \
                 "$PREFIX/share/metainfo/com.fukuyori.mdv.metainfo.xml"
fi
command -v update-desktop-database >/dev/null 2>&1 && \
  update-desktop-database -q "$PREFIX/share/applications" || true
echo "Installed mdv to $PREFIX (ensure $PREFIX/bin is on your PATH)."
EOF

  cat > "$stage/uninstall.sh" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
PREFIX="${PREFIX:-$HOME/.local}"
rm -f "$PREFIX/bin/mdv" \
      "$PREFIX/share/applications/mdv.desktop" \
      "$PREFIX/share/icons/hicolor/scalable/apps/mdv.svg" \
      "$PREFIX/share/metainfo/com.fukuyori.mdv.metainfo.xml"
echo "Removed mdv from $PREFIX."
EOF
  chmod 755 "$stage/install.sh" "$stage/uninstall.sh"

  local out="$DIST_DIR/${name}.tar.gz"
  tar -czf "$out" -C "$DIST_DIR/tarball" "$name"
  log "Created $out"
}

# -------------------------------------------------------------------------
# AppImage (self-contained; bundles Qt via linuxdeploy-plugin-qt).
# -------------------------------------------------------------------------
build_appimage() {
  local ld qtplugin
  ld="$(command -v linuxdeploy || command -v linuxdeploy-x86_64.AppImage || true)"
  qtplugin="$(command -v linuxdeploy-plugin-qt || command -v linuxdeploy-plugin-qt-x86_64.AppImage || true)"
  if [[ -z "$ld" || -z "$qtplugin" ]]; then
    warn "linuxdeploy and/or linuxdeploy-plugin-qt not found; skipping AppImage."
    warn "Get them from https://github.com/linuxdeploy/linuxdeploy/releases"
    return
  fi

  local appdir out
  appdir="$DIST_DIR/AppDir"
  rm -rf "$appdir"
  log "Building AppImage"
  stage_tree "$appdir"

  # linuxdeploy-plugin-qt needs to know where Qt lives if qmake isn't on PATH.
  local qmake
  qmake="$(command -v qmake6 || command -v qmake || true)"
  [[ -n "$qmake" ]] && export QMAKE="$qmake"
  # WebEngine ships a separate helper process that must be bundled explicitly.
  export EXTRA_QT_MODULES="webenginewidgets webenginecore webengine"

  mkdir -p "$DIST_DIR"
  (
    cd "$DIST_DIR"
    OUTPUT="mdv-${VERSION}-x86_64.AppImage" \
      "$ld" --appdir "$appdir" \
        --desktop-file "$appdir/usr/share/applications/mdv.desktop" \
        --icon-file "$appdir/usr/share/icons/hicolor/scalable/apps/mdv.svg" \
        --plugin qt \
        --output appimage
  )
  out="$DIST_DIR/mdv-${VERSION}-x86_64.AppImage"
  [[ -f "$out" ]] && log "Created $out" || warn "AppImage step finished but no output found."
}

# -------------------------------------------------------------------------
# Main
# -------------------------------------------------------------------------
formats=("$@")
if [[ ${#formats[@]} -eq 0 ]]; then
  formats=(deb tarball)
elif [[ "${formats[0]}" == "all" ]]; then
  formats=(deb tarball appimage)
fi

ensure_binary
mkdir -p "$DIST_DIR"
log "Packaging mdv $VERSION -> $DIST_DIR"

for fmt in "${formats[@]}"; do
  case "$fmt" in
    deb)      build_deb ;;
    tarball)  build_tarball ;;
    appimage) build_appimage ;;
    *)        die "unknown format: $fmt (expected: deb, tarball, appimage, all)" ;;
  esac
done

log "Done."
