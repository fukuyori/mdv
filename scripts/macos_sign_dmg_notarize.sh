#!/usr/bin/env bash
set -euo pipefail

if [[ "$(uname -s)" != "Darwin" ]]; then
  echo "This script only runs on macOS." >&2
  exit 1
fi

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-"$ROOT_DIR/build-release"}"
DIST_DIR="${DIST_DIR:-"$ROOT_DIR/dist/macos"}"
STAGE_DIR="$DIST_DIR/stage"
APP_NAME="${APP_NAME:-mdv}"
VERSION="${VERSION:-"$(awk '/project\(mdv VERSION/ { print $3; exit }' "$ROOT_DIR/CMakeLists.txt")"}"
BUNDLE_ID="${BUNDLE_ID:-com.fukuyori.mdv}"
NOTARY_PROFILE="${NOTARY_PROFILE:-notarytool}"
ENTITLEMENTS="${ENTITLEMENTS:-"$ROOT_DIR/scripts/macos/entitlements.plist"}"
APP_SOURCE="$BUILD_DIR/$APP_NAME.app"
APP_PATH="$STAGE_DIR/$APP_NAME.app"
DMG_RW="$DIST_DIR/$APP_NAME-$VERSION-rw.dmg"
DMG_FINAL="$DIST_DIR/$APP_NAME-$VERSION.dmg"

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

if [[ ! -d "$APP_SOURCE" ]]; then
  echo "App bundle not found: $APP_SOURCE" >&2
  echo "Run scripts/macos_build_release.sh first." >&2
  exit 1
fi

command -v macdeployqt >/dev/null 2>&1 || {
  echo "macdeployqt not found. Install Qt tools or add them to PATH." >&2
  exit 1
}

rm -rf "$DIST_DIR"
mkdir -p "$STAGE_DIR"
cp -R "$APP_SOURCE" "$APP_PATH"

macdeployqt "$APP_PATH" -verbose=1 -no-plugins

qt_plugin_dir() {
  if command -v qtpaths >/dev/null 2>&1; then
    qtpaths --plugin-dir
    return
  fi
  if command -v qtpaths6 >/dev/null 2>&1; then
    qtpaths6 --plugin-dir
    return
  fi
  echo "/opt/homebrew/share/qt/plugins"
}

copy_plugin_if_present() {
  local relative_path="$1"
  local plugin_root="$2"
  local source="$plugin_root/$relative_path"
  local destination="$APP_PATH/Contents/PlugIns/$relative_path"

  if [[ ! -f "$source" ]]; then
    return
  fi

  mkdir -p "$(dirname "$destination")"
  cp "$source" "$destination"
  install_name_tool -add_rpath "@loader_path/../../Frameworks" "$destination" >/dev/null 2>&1 || true
}

deploy_required_qt_plugins() {
  local plugin_root
  plugin_root="$(qt_plugin_dir)"

  copy_plugin_if_present "platforms/libqcocoa.dylib" "$plugin_root"
  copy_plugin_if_present "styles/libqmacstyle.dylib" "$plugin_root"
  copy_plugin_if_present "imageformats/libqgif.dylib" "$plugin_root"
  copy_plugin_if_present "imageformats/libqicns.dylib" "$plugin_root"
  copy_plugin_if_present "imageformats/libqico.dylib" "$plugin_root"
  copy_plugin_if_present "imageformats/libqjpeg.dylib" "$plugin_root"
  copy_plugin_if_present "imageformats/libqwebp.dylib" "$plugin_root"
}

sign_path() {
  local path="$1"
  codesign --force --timestamp --options runtime \
    --entitlements "$ENTITLEMENTS" \
    --sign "$CODESIGN_IDENTITY" "$path"
}

remove_signature_if_present() {
  local path="$1"
  codesign --remove-signature "$path" >/dev/null 2>&1 || true
}

is_macho() {
  file "$1" | grep -q "Mach-O"
}

sign_macho_files() {
  while IFS= read -r path; do
    if is_macho "$path"; then
      remove_signature_if_present "$path"
      sign_path "$path"
    fi
  done < <(find "$APP_PATH/Contents" -type f -print | awk '{ print length, $0 }' | sort -rn | cut -d' ' -f2-)
}

sign_bundle_dirs() {
  while IFS= read -r path; do
    remove_signature_if_present "$path"
    sign_path "$path"
  done < <(find "$APP_PATH/Contents" \( -name "*.framework" -o -name "*.app" \) -type d -print | awk '{ print length, $0 }' | sort -rn | cut -d' ' -f2-)
}

deploy_required_qt_plugins
sign_macho_files
sign_bundle_dirs

sign_path "$APP_PATH"
codesign --verify --deep --strict --verbose=2 "$APP_PATH"
spctl --assess --type execute --verbose=2 "$APP_PATH" || true

ln -s /Applications "$STAGE_DIR/Applications"
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
