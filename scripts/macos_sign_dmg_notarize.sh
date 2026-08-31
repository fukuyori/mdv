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
BUNDLE_ID="${BUNDLE_ID:-com.fukuyori.mdv}"
NOTARY_PROFILE="${NOTARY_PROFILE:-notarytool}"

# This script recursively replaces its output directory. Resolve symlinks and
# refuse any override outside the repository's dedicated dist tree.
mkdir -p "$ROOT_DIR/dist" "$DIST_DIR"
SAFE_DIST_ROOT="$(cd "$ROOT_DIR/dist" && pwd -P)"
DIST_DIR="$(cd "$DIST_DIR" && pwd -P)"
case "$DIST_DIR/" in
  "$SAFE_DIST_ROOT/"*) ;;
  *) echo "DIST_DIR must be inside $SAFE_DIST_ROOT: $DIST_DIR" >&2; exit 1 ;;
esac

STAGE_DIR="$DIST_DIR/stage"
APP_PATH="$STAGE_DIR/$APP_NAME.app"
DMG_BASENAME="${DMG_BASENAME:-${APP_NAME}-${VERSION}-macos-arm}"
DMG_RW="$DIST_DIR/$DMG_BASENAME-rw.dmg"
DMG_FINAL="$DIST_DIR/$DMG_BASENAME.dmg"

detect_identity() {
  security find-identity -v -p codesigning \
    | awk -F '"' '/Developer ID Application/ { print $2; exit }'
}

CODESIGN_IDENTITY="${CODESIGN_IDENTITY:-"$(detect_identity)"}"
if [[ -z "$CODESIGN_IDENTITY" ]]; then
  echo "Developer ID Application identity not found." >&2
  echo "Set CODESIGN_IDENTITY=\"Developer ID Application: ...\" and retry." >&2
  exit 1
fi

# The deployed and signed bundle comes from macos_sign_app.sh, which
# macos_package_pkg.sh runs the same way. Set SKIP_SIGN_APP=1 to reuse an
# existing stage instead of deploying and signing the app again.
if [[ "${SKIP_SIGN_APP:-0}" == "1" ]]; then
  if [[ ! -d "$APP_PATH" ]]; then
    echo "SKIP_SIGN_APP=1 but no signed app is staged: $APP_PATH" >&2
    echo "Run scripts/macos_sign_app.sh first." >&2
    exit 1
  fi
else
  DIST_DIR="$DIST_DIR" APP_NAME="$APP_NAME" CODESIGN_IDENTITY="$CODESIGN_IDENTITY" \
    "$ROOT_DIR/scripts/macos_sign_app.sh"
fi

rm -f "$DMG_RW" "$DMG_FINAL"
ln -sfn /Applications "$STAGE_DIR/Applications"
hdiutil create -volname "$APP_NAME $VERSION" \
  -srcfolder "$STAGE_DIR" \
  -format UDRW \
  -fs HFS+ \
  "$DMG_RW"

hdiutil convert "$DMG_RW" -format UDZO -imagekey zlib-level=9 -o "$DMG_FINAL"
rm -f "$DMG_RW"

codesign --force --timestamp --sign "$CODESIGN_IDENTITY" "$DMG_FINAL"
codesign --verify --verbose=2 "$DMG_FINAL"

if [[ "${SKIP_NOTARIZE:-0}" == "1" ]]; then
  echo "Skipping notarization because SKIP_NOTARIZE=1"
  echo "Created and signed: $DMG_FINAL"
  exit 0
fi

xcrun notarytool submit "$DMG_FINAL" \
  --keychain-profile "$NOTARY_PROFILE" \
  --wait

xcrun stapler staple "$DMG_FINAL"
xcrun stapler validate "$DMG_FINAL"
spctl --assess --type open --verbose=2 "$DMG_FINAL" || true

echo "Created, signed, notarized, and stapled: $DMG_FINAL"
