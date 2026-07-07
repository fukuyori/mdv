# mdv

Simple Markdown viewer/editor built with C++ and Qt Widgets.

English | [日本語](README.ja.md) | [Changelog](CHANGELOG.md)

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
- Translate the preview with a local [Ollama](https://ollama.com) server:
  switch between original, bilingual (original and translation interleaved),
  and translation-only views with buttons above the preview
  (see [Translation](#translation))
- Switch the UI language between English and Japanese
- Show or hide the editor pane

## Build

Requires Qt with the WebEngine, WebChannel, and Network modules (`brew install qt` on macOS).

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

## Translation

The preview can be translated with a local [Ollama](https://ollama.com)
server. Install Ollama, pull a model (for example
`ollama pull translategemma`), and make sure the server is running.

Use the buttons above the preview (or the Translation menu) to switch views:

- **Original** - the plain preview
- **Bilingual** - each block of the original followed by its translation
- **Translation** - translated text only

Translation > Translation Settings configures the endpoint (default
`http://127.0.0.1:11434`), the model (installed models are listed
automatically), the target language, and the number of parallel requests
(1-8; values above 1 only help when the server itself is started with
`OLLAMA_NUM_PARALLEL` greater than 1).

Blocks are translated in reading order starting from the part currently on
screen, results appear as they arrive, and translations are cached per
block, so editing retranslates only the blocks that changed. A block whose
translation fails is shown untranslated with a "(translation failed)"
marker in the bilingual view; the rest of the document continues.

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

The DMG is written as `dist/macos/mdv-<version>-macos-arm.dmg`.

The notarization script uses the saved notarytool profile named `notarytool` by default. Override signing settings with environment variables when needed:

```sh
CODESIGN_IDENTITY="Developer ID Application: Name (TEAMID)" \
NOTARY_PROFILE="notarytool" \
scripts/macos_sign_dmg_notarize.sh
```

The hardened-runtime entitlements in `scripts/macos/entitlements.plist`
include the JIT entitlements that Qt WebEngine (Chromium) requires.

Windows Release build:

```powershell
.\scripts\build-windows.ps1
```

This creates a Release build and deploys the Qt runtime with `windeployqt`
under `dist\mdv-windows-x64`.

Create a Windows installer with Inno Setup:

```powershell
.\scripts\package-windows-inno.ps1
```

The installer script reads the version from `CMakeLists.txt`, uses the
existing payload under `dist\mdv-windows-x64`, generates
`build-inno-installer\mdv.iss`, and writes `dist\mdv-<version>-windows-x64.exe`.
Run `.\scripts\build-windows.ps1` first, or pass `-Build` when you explicitly
want packaging to rebuild the payload. Use `-GenerateOnly` to generate the
`.iss` file without invoking Inno Setup.
