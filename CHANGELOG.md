# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

English | [日本語](CHANGELOG.ja.md)

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
