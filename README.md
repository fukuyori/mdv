# mdv

Simple Markdown viewer/editor built with C++ and Qt Widgets.

English | [日本語](README.ja.md)

## Screenshots

Editor with live preview:

![mdv editor and preview](images/screenshot1.png)

Viewer mode (`-v`), with the editor pane hidden:

![mdv viewer mode](images/screenshot2.png)

## Features

- Edit Markdown in the left pane
- Live GitHub Flavored Markdown preview (tables, task lists, strikethrough,
  autolinks, raw HTML) rendered with Qt WebEngine + [md4c](https://github.com/mity/md4c)
- Two-way synchronized scrolling between the editor and the preview,
  anchored on headings
- Browse headings in a left-side outline and jump to them
- Anchor links to headings (`#section`) work inside the preview;
  external links open in the system browser
- Create, open, save, and save-as Markdown files
- Encoding safety: files are read as UTF-8 (UTF-16/32 with BOM are detected
  and preserved on save), and a warning is shown before opening or saving a
  file that did not decode cleanly
- Confirmation prompt before opening files larger than 10 MB
- Reopen files from the recent files menu
- Find and replace text, with matches highlighted in both the editor and
  the preview (all matches marked, current match emphasized and scrolled to)
- Undo, redo, cut, copy, and paste text
- Paste clipboard images or copied image files as Markdown image links
- Switch between light, dark, and sepia themes
- Change editor, outline, and preview font size
- Choose editor and preview fonts
- Switch the UI language between English and Japanese
- Show or hide the editor pane

## Build

Requires Qt with the WebEngine and WebChannel modules (`brew install qt` on macOS).

```sh
cmake -S . -B build
cmake --build build
```

## Run

macOS:

```sh
open build/mdv.app
```

Other platforms:

```sh
./build/mdv
```

Open a file directly by passing it as an argument:

```sh
open -a mdv README.md                        # macOS (installed app)
build/mdv.app/Contents/MacOS/mdv README.md   # macOS (direct binary)
./build/mdv README.md                        # other platforms
```

On macOS the app also accepts files from Finder ("Open With") and from
drops onto its Dock icon.

Start in viewer mode, with the editor pane hidden:

```sh
open build/mdv.app --args -v   # macOS
./build/mdv -v                 # other platforms
```

The Open and Save As dialogs start in the current working directory (home
when the app is launched from Finder) and then follow the directory you
last used; Save As keeps only the file name of the open document, so an
opened file's location never overrides your chosen save directory.

## Project Structure

```
src/main.cpp        Application (window, editor, preview pipeline, sync)
third_party/md4c/   Vendored md4c Markdown parser (MIT license)
resources/          App icon sources and macOS icon set
scripts/            macOS release, signing, and icon generation scripts
tools/icon_renderer SVG-to-PNG helper used by the icon script
```

### Preview pipeline

The editor text is converted to HTML with md4c (GitHub dialect) and pushed
into a `QWebEngineView` that loads a themed HTML template once; subsequent
updates replace only the page content, debounced at 120 ms, so the preview
neither flickers nor loses its scroll position while typing. Scroll positions
are mapped between the panes by pairing headings and interpolating inside
each segment; the preview reports its own scrolls back over `QWebChannel`.
Clicked links never navigate the preview: http/https/mailto URLs open in the
system browser and every other scheme is blocked.

## Release Build

Build a Release app bundle on macOS:

```sh
scripts/macos_build_release.sh
```

Regenerate the macOS app icon from `resources/icon.svg`:

```sh
scripts/macos_generate_icon.sh
```

Sign, create a DMG, submit it for notarization, and staple the notarization ticket:

```sh
scripts/macos_sign_dmg_notarize.sh
```

The notarization script uses the saved notarytool profile named `notarytool` by default. Override signing settings with environment variables when needed:

```sh
CODESIGN_IDENTITY="Developer ID Application: Name (TEAMID)" \
NOTARY_PROFILE="notarytool" \
scripts/macos_sign_dmg_notarize.sh
```

The hardened-runtime entitlements in `scripts/macos/entitlements.plist`
include the JIT entitlements that Qt WebEngine (Chromium) requires.
