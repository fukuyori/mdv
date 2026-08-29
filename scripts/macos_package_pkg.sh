#!/usr/bin/env bash
set -euo pipefail

if [[ "$(uname -s)" != "Darwin" ]]; then
  echo "This script only runs on macOS." >&2
  exit 1
fi

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DIST_DIR="${DIST_DIR:-"$ROOT_DIR/dist/macos"}"
APP_NAME="${APP_NAME:-mdv}"
VERSION="${VERSION:-"$(awk '/project\(mdv VERSION/ { print $3; exit }' "$ROOT_DIR/CMakeLists.txt")"}"
PKG_ID="${PKG_ID:-com.fukuyori.mdv}"
NOTARY_PROFILE="${NOTARY_PROFILE:-notarytool}"
CLI_NAME="${CLI_NAME:-mdv}"
CLI_DIR="${CLI_DIR:-/usr/local/bin}"

# Same containment rule as macos_sign_dmg_notarize.sh: this script recursively
# replaces directories under DIST_DIR, so refuse any override outside the
# repository's dedicated dist tree.
mkdir -p "$ROOT_DIR/dist" "$DIST_DIR"
SAFE_DIST_ROOT="$(cd "$ROOT_DIR/dist" && pwd -P)"
DIST_DIR="$(cd "$DIST_DIR" && pwd -P)"
case "$DIST_DIR/" in
  "$SAFE_DIST_ROOT/"*) ;;
  *) echo "DIST_DIR must be inside $SAFE_DIST_ROOT: $DIST_DIR" >&2; exit 1 ;;
esac

STAGE_DIR="$DIST_DIR/stage"
APP_SOURCE="${APP_SOURCE:-"$STAGE_DIR/$APP_NAME.app"}"
PKG_ROOT="$DIST_DIR/pkgroot"
PKG_SCRIPTS="$DIST_DIR/pkgscripts"
PKG_COMPONENT="$DIST_DIR/$APP_NAME-component.pkg"
PKG_COMPONENT_PLIST="$DIST_DIR/$APP_NAME-component.plist"
PKG_BASENAME="${PKG_BASENAME:-${APP_NAME}-${VERSION}-macos-arm}"
PKG_FINAL="$DIST_DIR/$PKG_BASENAME.pkg"

detect_installer_identity() {
  # "Developer ID Installer" is not a codesigning identity, so unlike
  # macos_sign_dmg_notarize.sh this must not pass -p codesigning.
  security find-identity -v \
    | awk -F '"' '/Developer ID Installer/ { print $2; exit }'
}

INSTALLER_IDENTITY="${INSTALLER_IDENTITY:-"$(detect_installer_identity)"}"
if [[ -z "$INSTALLER_IDENTITY" ]]; then
  echo "Developer ID Installer identity not found." >&2
  echo "Set INSTALLER_IDENTITY=\"Developer ID Installer: ...\" and retry." >&2
  echo "Note that this is a different certificate from the Developer ID" >&2
  echo "Application identity used to sign the app bundle." >&2
  exit 1
fi

# The deployed and signed bundle comes from macos_sign_app.sh, which
# macos_sign_dmg_notarize.sh runs the same way, so a pkg can be built without
# producing a DMG at all. Set SKIP_SIGN_APP=1 to reuse an existing stage —
# chaining both scripts otherwise deploys and signs the app twice — or point
# APP_SOURCE at a bundle that is already signed.
if [[ "${SKIP_SIGN_APP:-0}" != "1" && "$APP_SOURCE" == "$STAGE_DIR/$APP_NAME.app" ]]; then
  DIST_DIR="$DIST_DIR" APP_NAME="$APP_NAME" \
    "$ROOT_DIR/scripts/macos_sign_app.sh"
fi

if [[ ! -d "$APP_SOURCE" ]]; then
  echo "Signed app bundle not found: $APP_SOURCE" >&2
  echo "Run scripts/macos_sign_app.sh first." >&2
  exit 1
fi

# The pkg only wraps an already signed and hardened bundle; signing it here
# would bypass the deployment fixups that macos_sign_app.sh performs.
if ! codesign --verify --deep --strict "$APP_SOURCE" 2>/dev/null; then
  echo "App bundle is not validly signed: $APP_SOURCE" >&2
  echo "Run scripts/macos_sign_app.sh first." >&2
  exit 1
fi

rm -rf "$PKG_ROOT" "$PKG_SCRIPTS" "$PKG_COMPONENT" "$PKG_COMPONENT_PLIST" "$PKG_FINAL"
mkdir -p "$PKG_ROOT" "$PKG_SCRIPTS"

# The payload root holds only the bundle, with /Applications supplied through
# --install-location. Staging it as pkgroot/Applications/mdv.app instead would
# put /Applications itself in the bill of materials, and Installer would reset
# the real directory from root:admin drwxrwxr-x to the staged root:wheel.
#
# ditto rather than cp -R so the code signature and its extended attributes
# survive the copy into the payload root.
ditto "$APP_SOURCE" "$PKG_ROOT/$APP_NAME.app"

# An upgrade install merges the payload into an existing bundle instead of
# replacing it, so files dropped between releases (a renamed Qt dylib, say)
# would survive inside the new bundle and invalidate its sealed signature.
# Remove the old bundle first. This is safe only because the component plist
# below turns off the version check, which could otherwise skip the payload
# after preinstall has already deleted the app.
cat > "$PKG_SCRIPTS/preinstall" <<EOF
#!/bin/sh
set -e

if [ -d "/Applications/$APP_NAME.app" ]; then
  rm -rf "/Applications/$APP_NAME.app"
fi
EOF
chmod 755 "$PKG_SCRIPTS/preinstall"

# The launcher is written by postinstall instead of shipped as payload. A
# payload file under /usr/local/bin would also put /usr/local and
# /usr/local/bin in the bill of materials, and Installer would then reset those
# existing directories to root:wheel — which breaks a Homebrew prefix there.
# mkdir -p leaves an existing directory's ownership alone.
#
# The launcher is a wrapper rather than a symlink to the bundle executable: Qt
# derives its plugin search path from the executable's own path, so a symlink
# would never find Contents/PlugIns and would abort with "Could not find the Qt
# platform plugin". exec keeps the real path and passes the caller's working
# directory through, which --session needs to locate the current project's log.
cat > "$PKG_SCRIPTS/postinstall" <<EOF
#!/bin/sh
set -e

mkdir -p "$CLI_DIR"
cat > "$CLI_DIR/$CLI_NAME" <<'LAUNCHER'
#!/bin/sh
exec "/Applications/$APP_NAME.app/Contents/MacOS/$APP_NAME" "\$@"
LAUNCHER
chmod 755 "$CLI_DIR/$CLI_NAME"
EOF
chmod 755 "$PKG_SCRIPTS/postinstall"

# pkgbuild treats an app bundle as relocatable by default, so Installer would
# follow an existing copy of mdv.app elsewhere on the disk and install there
# instead of /Applications — leaving the launcher pointing at nothing. The
# default version check would also silently skip the payload when a newer build
# is already installed, which must not happen once preinstall has removed it.
pkgbuild --analyze --root "$PKG_ROOT" "$PKG_COMPONENT_PLIST" >/dev/null
/usr/libexec/PlistBuddy \
  -c "Set :0:BundleIsRelocatable false" \
  -c "Set :0:BundleIsVersionChecked false" \
  "$PKG_COMPONENT_PLIST" >/dev/null

# --ownership recommended installs the payload as root:wheel even though the
# package is built by an unprivileged user.
pkgbuild \
  --root "$PKG_ROOT" \
  --component-plist "$PKG_COMPONENT_PLIST" \
  --scripts "$PKG_SCRIPTS" \
  --install-location /Applications \
  --ownership recommended \
  --identifier "$PKG_ID" \
  --version "$VERSION" \
  "$PKG_COMPONENT"

productbuild \
  --package "$PKG_COMPONENT" \
  --sign "$INSTALLER_IDENTITY" \
  "$PKG_FINAL"

rm -rf "$PKG_COMPONENT" "$PKG_COMPONENT_PLIST" "$PKG_ROOT" "$PKG_SCRIPTS"

pkgutil --check-signature "$PKG_FINAL"

if [[ "${SKIP_NOTARIZE:-0}" == "1" ]]; then
  echo "Skipping notarization because SKIP_NOTARIZE=1"
  echo "Created and signed: $PKG_FINAL"
  exit 0
fi

xcrun notarytool submit "$PKG_FINAL" \
  --keychain-profile "$NOTARY_PROFILE" \
  --wait

xcrun stapler staple "$PKG_FINAL"
xcrun stapler validate "$PKG_FINAL"
spctl --assess --type install --verbose=2 "$PKG_FINAL" || true

echo "Created, signed, notarized, and stapled: $PKG_FINAL"
