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
find "$APP_PATH" -name "_CodeSignature" -type d -prune -exec rm -rf {} +

macdeployqt "$APP_PATH" -verbose=1 -no-plugins -no-codesign
find "$APP_PATH" -name "_CodeSignature" -type d -prune -exec rm -rf {} +

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

# macdeployqt (against Homebrew's modular Qt) leaves absolute /opt/homebrew
# load commands in QtWebEngineProcess and in the plugins we copy ourselves,
# and rewrites the rest to @executable_path/../Frameworks. Under the hardened
# runtime dyld rejects the Homebrew libraries (different Team ID), and the
# @executable_path form only resolves from the main executable, not from the
# QtWebEngineProcess helper — either way the renderer process dies and the
# preview pane stays blank. Rewrite every such reference to @rpath and bundle
# any non-Qt dylibs they depend on.
FRAMEWORKS_DIR="$APP_PATH/Contents/Frameworks"
EXEC_PREFIX="@executable_path/../Frameworks/"

homebrew_refs() {
  otool -L "$1" | awk '/\/opt\/homebrew\/|@executable_path\/\.\.\/Frameworks\//{ print $1 }'
}

rpath_name_for() {
  local ref="$1"
  if [[ "$ref" == "$EXEC_PREFIX"* ]]; then
    echo "@rpath/${ref#"$EXEC_PREFIX"}"
  elif [[ "$ref" == *".framework/"* ]]; then
    echo "@rpath/${ref#*/lib/}"
  else
    echo "@rpath/$(basename "$ref")"
  fi
}

fix_homebrew_references() {
  local pending=()
  local file ref new_ref own_id bundled
  while IFS= read -r file; do
    if is_macho "$file"; then
      pending+=("$file")
    fi
  done < <(find "$APP_PATH/Contents" -type f -print)

  while ((${#pending[@]} > 0)); do
    file="${pending[0]}"
    pending=("${pending[@]:1}")
    own_id="$(otool -D "$file" 2>/dev/null | sed -n '2p')"
    while IFS= read -r ref; do
      [[ -n "$ref" ]] || continue
      new_ref="$(rpath_name_for "$ref")"
      if [[ "$ref" == "$EXEC_PREFIX"* || "$ref" == *".framework/"* ]]; then
        local bundled_name="${new_ref#@rpath/}"
        if [[ ! -e "$FRAMEWORKS_DIR/$bundled_name" ]]; then
          echo "Library referenced by $file is not bundled: $bundled_name" >&2
          exit 1
        fi
      else
        bundled="$FRAMEWORKS_DIR/$(basename "$ref")"
        if [[ ! -f "$bundled" ]]; then
          if [[ ! -e "$ref" ]]; then
            echo "Missing dylib referenced by $file: $ref" >&2
            exit 1
          fi
          cp "$ref" "$bundled"
          chmod 644 "$bundled"
          install_name_tool -id "$new_ref" "$bundled"
          pending+=("$bundled")
        fi
      fi
      if [[ "$ref" == "$own_id" ]]; then
        install_name_tool -id "$new_ref" "$file"
      else
        install_name_tool -change "$ref" "$new_ref" "$file"
      fi
    done < <(homebrew_refs "$file")
  done
}

normalize_rpaths() {
  local main_exe="$APP_PATH/Contents/MacOS/$APP_NAME"
  local file rpath
  while IFS= read -r file; do
    is_macho "$file" || continue
    while IFS= read -r rpath; do
      install_name_tool -delete_rpath "$rpath" "$file"
    done < <(otool -l "$file" | awk '/LC_RPATH/{ getline; getline; if ($2 ~ /^\/opt\/homebrew\//) print $2 }')
  done < <(find "$APP_PATH/Contents" -type f -print)

  if ! otool -l "$main_exe" | grep -q "@executable_path/\.\./Frameworks"; then
    install_name_tool -add_rpath "@executable_path/../Frameworks" "$main_exe"
  fi
}

verify_no_homebrew_references() {
  local file leftover
  while IFS= read -r file; do
    is_macho "$file" || continue
    leftover="$(homebrew_refs "$file")"
    if [[ -n "$leftover" ]]; then
      echo "Unresolved /opt/homebrew or @executable_path references remain in $file:" >&2
      echo "$leftover" >&2
      exit 1
    fi
  done < <(find "$APP_PATH/Contents" -type f -print)
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
fix_homebrew_references
normalize_rpaths
verify_no_homebrew_references
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
