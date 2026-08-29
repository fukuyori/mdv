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
- Markdown-aware input helpers: auto-indent on Enter, automatic continuation
  of list and quote markers (bullets, numbered lists, checkboxes, quotes),
  Shift+Enter for a plain newline, and Tab / Shift+Tab to indent or unindent
  selected lines
- Live GitHub Flavored Markdown preview (tables, task lists, strikethrough,
  autolinks, raw HTML) rendered with Qt WebEngine + [md4c](https://github.com/mity/md4c)
- Copy selected preview text from its context menu, or copy an entire code
  block with the button shown on the block
- Two-way synchronized scrolling between the editor and the preview,
  anchored on headings
- Browse headings in a left-side outline and jump to them
- Anchor links to headings (`#section`) work inside the preview;
  external links open in the system browser
- Create, open, save, and save-as Markdown files
- Tabs: edit multiple Markdown files at once, each with its own editor,
  outline, and preview. Open several files at once from the command line or
  by dropping them onto the window; opening a file that's already open
  switches to its tab instead of duplicating it
- Multiple windows: right-click a tab for "Open in New Window", or drag a
  tab out of the tab bar to detach it into a new window. Dragging a tab
  onto another mdv window moves it there instead
- Detects when a tab's file changes on disk (edited by another program) and
  offers to reload it
- Follow mode (`-f`) automatically reloads files as they grow and keeps the
  preview pinned to the last line, with the editor pane hidden
- Encoding safety: files are read as UTF-8 (UTF-16/32 with BOM are detected
  and preserved on save), and a warning is shown before opening or saving a
  file that did not decode cleanly
- Confirmation prompt before opening files larger than 10 MB
- Reopen files from the recent files menu
- Find and replace text, with matches highlighted in both the editor and
  the preview (all matches marked, current match emphasized and scrolled to)
- Undo, redo, cut, copy, and paste text
- Paste clipboard images or copied image files as Markdown image links
- Render Mermaid diagrams and inline or display LaTeX math directly in the
  preview, using bundled offline libraries
- Highlight fenced code blocks according to their language
- Render GitHub-style note, tip, important, warning, and caution alerts
- Switch between light, dark, and sepia themes
- Change editor, outline, and preview font size
- Choose editor and preview fonts
- Translate the preview with a local [Ollama](https://ollama.com) server:
  switch between original, bilingual (original and translation interleaved),
  and translation-only views with buttons above the preview
  (see [Translation](#translation)). Selected preview text can also be
  translated independently from its context menu, including in follow mode
- Switch the UI language between English and Japanese
- Show or hide the editor pane with a slim toggle beside the preview, or from
  the View menu

## Mermaid and math

Mermaid diagrams use a fenced code block with the `mermaid` language:

````markdown
```mermaid
flowchart LR
    A[Markdown] --> B[Preview]
```
````

LaTeX math uses `$...$` inline and `$$...$$` for a display equation:

```markdown
Euler's identity is $e^{i\pi} + 1 = 0$.

$$
\int_{-\infty}^{\infty} e^{-x^2}\,dx = \sqrt{\pi}
$$
```

Mermaid and KaTeX are bundled with mdv, so both features work without an
internet connection. Invalid Mermaid syntax remains visible as source with an
error message; invalid math remains visible instead of breaking the preview.

## Syntax highlighting and alerts

Add a language after the opening fence to highlight a code block:

````markdown
```cpp
#include <iostream>

int main() {
    std::cout << "Hello\n";
}
```
````

GitHub-style alerts use a blockquote whose first line contains one of
`NOTE`, `TIP`, `IMPORTANT`, `WARNING`, or `CAUTION`:

```markdown
> [!NOTE]
> This is useful supplementary information.

> [!WARNING]
> Check the destination before overwriting a file.
```

The bundled Highlight.js common-language build and its light, dark, and sepia
styles work offline. Alert titles are displayed in the selected UI language.

For security, raw HTML is shown as text rather than executed, and remote images
or other remote resources are not loaded automatically. Open external links
only after an explicit click. Normal mode accepts files up to 256 MiB; use
`-f` for larger append-only logs.

## Build

### Requirements

- CMake 3.16 or newer
- A C++17 compiler
- Qt 6.10.3 or newer with Widgets, WebEngine, WebChannel, and Network

The current release is built and tested with Qt 6.11.2. When using the Qt
Online Installer, install the Desktop kit for the compiler you intend to use
and include Qt WebEngine and Qt WebChannel. The installer resolves their
additional Qt dependencies. Build and packaging scripts read the shared
default version from `qt-default-version.txt`.

Qt 6.10.3 is also supported when staying on the 6.10 release series is more
important than using the newer tested version. Qt 6.10.2 is intentionally not
accepted because it predates required security fixes.

Do not reuse a build directory that was configured with a different Qt
installation. Select a new `BUILD_DIR`, as shown below, when changing Qt
versions.

### Linux

Install CMake, a C++ compiler, and Qt first. Keep the Qt libraries under `/usr` managed by the distribution package
manager. Do not overwrite them manually with an Online Installer build;
install a project-specific Qt under your user directory and select it with
`QT_ROOT`. Upgrade the system Qt only when the distribution provides the new
version through its normal package updates.

The build script uses `$HOME/Qt/6.11.2/gcc_64` by default, followed by the
same kit under `/opt/Qt` or `/usr/local/Qt`. It fails instead of silently using
an older system Qt when none of these paths exists. Build and test a Release
binary with:

```sh
BUILD_DIR=build-qt6112 \
scripts/linux_build_release.sh

ctest --test-dir build-qt6112 --output-on-failure
build-qt6112/mdv --version
```

The resulting executable is `build-qt6112/mdv`. `QT_ROOT` must be the Qt kit
directory containing `bin/qt-cmake`, not the parent version directory. Set it
only to override the 6.11.2 default, for example to use a separately installed
Qt 6.10.3:

```sh
QT_ROOT=/opt/Qt/6.10.3/gcc_64 scripts/linux_build_release.sh
```

#### Why keep the Qt installations separate?

- APT owns the Qt libraries and plugins under `/usr`. Replacing only part of
  that package set can mix incompatible libraries, plugins, and WebEngine
  helper processes while leaving dpkg's installed-file database incorrect.
- mdv renders untrusted Markdown with Qt WebEngine, so it benefits directly
  from fixes newer than the distribution's Qt 6.10.2.
- A separate Qt selected by `QT_ROOT` updates or rolls back mdv without
  changing the runtime used by other Ubuntu applications. When Ubuntu
  publishes a newer coherent Qt package set, the system Qt can be upgraded
  normally through APT.

For a manual CMake build or a non-Release configuration, use:

```sh
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_PREFIX_PATH=/home/user/Qt/6.11.2/gcc_64
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

### macOS

Install Qt 6.11.2 with the Qt Online Installer under `$HOME/Qt`. The release
script selects `$HOME/Qt/6.11.2/macos` by default:

```sh
scripts/macos_build_release.sh
ctest --test-dir build-release --output-on-failure
open build-release/mdv.app
```

Set `QT_ROOT` explicitly when using Homebrew or another Qt prefix:

```sh
brew install qt cmake
QT_ROOT="$(brew --prefix qt)" scripts/macos_build_release.sh
```

### Windows

Install Qt 6.10.3 or newer and either its MSVC or MinGW 64-bit Desktop kit.
The script selects a kit under `C:\Qt\6.11.2` by default:

```powershell
.\scripts\build-windows.ps1
ctest --test-dir build-windows-release -C Release --output-on-failure
dist\mdv-windows-x64\mdv.exe --version
```

The Windows script builds the application and runs `windeployqt`; the deployed
application is written to `dist\mdv-windows-x64`. Pass `-QtDir` to override
the default kit.

## Run

macOS:

```sh
open build/mdv.app
```

Other platforms:

```sh
./build/mdv
```

Open one or more files directly by passing them as arguments; each opens in
its own tab:

```sh
open -a mdv README.md CHANGELOG.md                        # macOS (installed app)
build/mdv.app/Contents/MacOS/mdv README.md CHANGELOG.md   # macOS (direct binary)
./build/mdv README.md CHANGELOG.md                        # other platforms
```

Files can also be dropped onto the window to open them as tabs. On macOS the
app additionally accepts files from Finder ("Open With") and from drops onto
its Dock icon.

Start in viewer mode, with the editor pane hidden:

```sh
open build/mdv.app --args -v   # macOS
./build/mdv -v                 # other platforms
```

In viewer mode, use the slim toggle on the left edge of the preview to show
the editor pane again.

Follow a file like `tail -f`, automatically reloading it and keeping its last
line at the bottom of the preview:

```sh
open build/mdv.app --args -f log.md   # macOS
./build/mdv -f log.md                 # other platforms
```

Follow mode is read-only: it keeps the editor pane hidden and disables save
and editing actions so an external writer cannot be overwritten. Bilingual
and translation-only views are also disabled because continuous translation
may not keep pace with incoming text; selected text can still be translated
on demand. Additional files opened in the same follow-mode window are followed
in the same way.
Only newly appended bytes are read after the initial load. The displayed tail
is bounded to the last 10,000 lines and at most 8 MiB of source data, so memory
use does not grow with the file; truncation, in-place overwrite, and file
replacement/rotation rebuild the bounded tail from the new file.

Scrolling away from the bottom pauses automatic scrolling, so incoming text
does not interrupt reading earlier content. Automatic following resumes when
you scroll back to the bottom or press `Esc`, which jumps directly to the
latest content.

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

To translate only part of the preview, select its text and choose **Translate
Selection** from the right-click menu or the Translation menu. The result
window shows both the selected source and its translation and provides a
button to copy the translation. This one-shot translation is also available
in viewer and follow (`-f`) modes.

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
third_party/mermaid Vendored Mermaid diagram renderer (MIT license)
third_party/katex/  Vendored KaTeX math renderer and fonts (MIT license)
third_party/highlightjs/ Vendored Highlight.js syntax highlighter (BSD-3-Clause)
resources/          App icon sources and macOS icon set
scripts/            macOS release, signing, and icon generation scripts
tools/icon_renderer SVG-to-PNG helper used by the icon script
```

### Preview pipeline

The editor text is converted to HTML with md4c (GitHub dialect plus LaTeX math
spans) and pushed
into a `QWebEngineView` that loads a themed HTML template once; subsequent
updates replace only the page content, debounced at 120 ms, so the preview
neither flickers nor loses its scroll position while typing. Scroll positions
are mapped between the panes by pairing headings and interpolating inside
each segment; the preview reports its own scrolls back over `QWebChannel`.
Bundled KaTeX renders md4c's math elements, while bundled Mermaid renders
`mermaid` code fences asynchronously; stale diagram results are discarded
when the document changes again. Bundled Highlight.js processes the remaining
code fences after each content update, and GitHub-style alert blockquotes are
classified and styled in the same DOM pass.
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

Authenticode signing on Windows:

```powershell
$env:CODESIGN_CERT = "C:\path\to\cert.pfx"
$env:CODESIGN_CERT_PASSWORD = "..."
.\scripts\build-windows.ps1 -Sign
.\scripts\package-windows-inno.ps1 -Sign
```

Both scripts take `-Sign` and read the certificate from the `CODESIGN_CERT`
environment variable, which accepts a `.pfx` path, a certificate SHA1
thumbprint, or a certificate subject name from the Windows certificate store.
`build-windows.ps1 -Sign` signs the deployed `mdv.exe`;
`package-windows-inno.ps1 -Sign` signs the payload `mdv.exe`, the installer,
and the uninstaller. Optional environment variables:
`CODESIGN_CERT_PASSWORD` (certificate file password),
`CODESIGN_TIMESTAMP_URL` (RFC 3161 server, default
`http://timestamp.digicert.com`), `CODESIGN_DIGEST` (default `sha256`),
`CODESIGN_CSP` and `CODESIGN_KEY_CONTAINER` (hardware tokens), and `SIGNTOOL`
(explicit `signtool.exe` path, also settable with `-SignToolPath`).
