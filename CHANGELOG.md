# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

English | [日本語](CHANGELOG.ja.md)

## [0.6.1] - 2026-08-29

### Fixed

- Follow mode now scrolls the outline to its final heading when a file is
  opened or reset after rotation, matching the preview's initial tail position.

## [0.6.0] - 2026-08-29

### Added

- Preview text selections can be translated independently from the context or
  Translation menu, with source/translation display and translation copying;
  this is available in normal, viewer, and follow modes.
- Follow mode (`-f`) opens files read-only with the editor pane hidden,
  automatically reloads changes, and keeps the last line at the bottom of
  the preview without risking overwrites from save actions. Bilingual and
  translation-only views are disabled in this mode while selection translation
  remains available. It reads appended bytes incrementally and bounds the
  displayed tail to 10,000 lines and 8 MiB. Scrolling back pauses automatic
  scrolling without losing the reading position; reaching the bottom or
  pressing `Esc` resumes following the latest content.

### Security

- Raw HTML in Markdown is now escaped instead of inserted into the active
  WebEngine DOM. The preview also applies a Content Security Policy and blocks
  automatic remote-resource access, clipboard JavaScript, and local storage.
- Updated Highlight.js from 11.11.1 to 11.12.0 to fix the XML grammar ReDoS;
  code blocks above 100,000 characters are still left unhighlighted as a
  general preview-availability safeguard.
- Updated the bundled MD4C parser from 0.5.2 to 0.5.3, including protections
  for pathological link-reference expansion, quadratic line lookup, and unsafe
  error paths.
- Single-instance IPC now uses per-user socket permissions, a stable per-user
  name, a 1 MiB message limit, and strict payload/path validation.
- Translation jobs retain their original endpoint/model/language, response
  bodies are bounded, invalid endpoint URLs are rejected, and plaintext remote
  endpoints require confirmation.
- Saving compares the backing file against its load-time SHA-256 digest before
  overwriting, and image paste refuses an existing `assets` symlink.
- Normal mode refuses files above 256 MiB and directs users to bounded follow
  mode. Packaging cleanup is confined to the repository `dist` directory, and
  Windows signing no longer accepts passwords that would appear in process
  command-line arguments.

### Changed

- Added compiler hardening flags and an automated security regression test for
  raw HTML and pathological link-reference expansion.
- Raised the minimum supported Qt version to 6.10.3, verified release builds
  with Qt 6.11.2, and added `QT_ROOT` selection to the Linux and macOS release
  build scripts.
- Release build and AppImage scripts now select Qt 6.11.2 by default and fail
  instead of silently falling back to an older system Qt; explicit Qt path
  overrides remain available on every platform. All scripts share the default
  through `qt-default-version.txt`.

## [0.5.3] - 2026-08-06

### Added

- Fenced code blocks are syntax-highlighted with bundled Highlight.js themes,
  and GitHub-style alerts are rendered for all five alert types.

## [0.5.2] - 2026-08-06

### Added

- Mermaid fenced code blocks and inline or display LaTeX math are rendered in
  the preview with bundled offline Mermaid and KaTeX libraries.

### Changed

- The initial untitled document is now empty instead of containing the built-in
  mdv introduction.

## [0.5.1] - 2026-07-30

### Fixed

- Clicking a heading in the outline now scrolls the rendered preview to the
  corresponding section, including when the editor pane is hidden.
- Linux application metadata is initialized before the GUI application,
  preventing duplicate host portal app ID registration warnings.

## [0.5.0] - 2026-07-28

### Added

- The preview context menu can copy selected rendered text.
- Every preview code block has an icon button that copies the entire block;
  the icon briefly changes to a check mark after copying.
- Ctrl+Q exits the application, including all open mdv windows.

### Fixed

- Model and target-language choices in Translation Settings remain readable
  when their drop-down lists are open in light, sepia, and dark themes.

## [0.4.0] - 2026-07-11

### Added

- Markdown-aware input helpers in the editor:
  - Pressing Enter carries the current line's indentation to the next line.
  - List and quote markers (`-` `*` `+`, numbered `1.` / `1)` with the next
    number, checkboxes `- [ ]`, and `>` quotes) continue automatically on
    the next line; pressing Enter on a marker-only line clears it and ends
    the list.
  - Shift+Enter inserts a plain newline (indentation only, no marker).
  - Tab indents all lines of a multi-line selection; Shift+Tab unindents
    the selected lines (or the current line without a selection).

## [0.3.2] - 2026-07-09

### Added

- A slim toggle beside the preview shows and hides the editor pane.

### Fixed

- Preview mode buttons now show their labels immediately when a tab is created.

## [0.3.1] - 2026-07-08

### Added

- Multiple windows: right-click a tab and choose "Open in New Window" to
  move it into a new window of its own (shown only when the window has more
  than one tab).
- Drag a tab out of the tab bar to detach it into a new window, with a
  translucent preview of the tab following the cursor while dragging.
- Dragging a tab and dropping it onto another mdv window moves the tab there
  instead of creating a new window. If the source window's last tab is
  moved this way, that window closes, leaving just the one window.

## [0.3.0] - 2026-07-08

### Added

- Tabs: open and edit multiple Markdown documents at once, each with its
  own editor, outline, and preview pane. New tabs can be created, closed
  (with an unsaved-changes prompt), and switched between with Ctrl+Tab /
  Ctrl+Shift+Tab.
- Opening a file that is already open in another tab switches to that tab
  instead of opening a duplicate.
- Multiple files passed on the command line, or dropped onto the window,
  now open as separate tabs (drag-and-drop file opening is new).
- Translation continues in background tabs; switching a tab back to the
  original view only cancels its own in-flight translations, not those of
  other tabs still translating the same content.
- Detect when the file behind a tab changes on disk (e.g. edited by another
  program) and offer to reload it: immediately for the active tab, or when
  a background tab is switched to. If the tab also has unsaved local edits
  and the reload is declined, the usual save-on-close prompt now explains
  that Save overwrites the external change and Discard keeps it.

## [0.2.2] - 2026-07-08

### Fixed

- macOS: the preview pane was always blank and the app could crash shortly
  after launch when an accessibility client (e.g. VoiceOver or Karabiner)
  queried the window. The signed app bundle shipped a QtWebEngineProcess
  helper that still referenced Homebrew Qt libraries by absolute path, so
  the Chromium renderer process could never start. The macOS packaging
  script now rewrites all library references inside the bundle to @rpath
  and bundles the missing image-format dependencies (libjpeg, libwebp).

## [0.2.1] - 2026-07-07

### Added

- `--version` command-line option prints the version to stdout.
- Version number shown permanently on the right side of the status bar.

## [0.2.0] - 2026-07-06

### Added

- Translate the preview through a local [Ollama](https://ollama.com) server.
  Three views are switchable with buttons above the preview or from the new
  Translation menu: original, bilingual (original and translation
  interleaved), and translation only.
- Translation settings dialog: Ollama endpoint, model (installed models are
  listed from the server), target language (Japanese / English), and number
  of parallel requests (1-8).
- Block-by-block translation with progressive display: translated blocks
  appear as results arrive, results are cached per block, and only edited
  blocks are retranslated.
- Translation order follows the reader: blocks at and below the current
  viewport are translated first, and the waiting queue is reordered as you
  scroll.
- Per-block error handling: a block that fails to translate (server error,
  timeout, empty response) is marked "(translation failed)" in the bilingual
  view and shown as the original text, without aborting the rest of the
  document. Translation stops entirely only when the server is unreachable.
- Linux release build and packaging scripts (deb / tarball).
- Application icon embedded on Linux.

### Fixed

- Font dialog mojibake on Linux.
- macOS release signing scripts.

## [0.1.2] - 2026-07-05

### Added

- Windows build and packaging scripts (Inno Setup installer) and Windows
  application icon.

## [0.1.1] - 2026-07-05

### Added

- Open files handed over by macOS: `open -a`, Finder "Open With", and drops
  onto the Dock icon.

## [0.1.0] - 2026-07-05

### Added

- Initial release: Markdown viewer/editor built with C++ and Qt Widgets.
  GitHub Flavored Markdown live preview (Qt WebEngine + md4c), two-way
  synchronized scrolling, heading outline, find and replace with preview
  highlighting, encoding safety checks, recent files, clipboard image
  pasting, light/dark/sepia themes, font selection, English/Japanese UI,
  and a viewer mode with the editor pane hidden.
