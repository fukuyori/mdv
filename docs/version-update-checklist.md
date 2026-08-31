# Version update checklist

Use this checklist whenever the mdv application version changes.

## Direct version values

- [x] `CMakeLists.txt`: update `project(mdv VERSION ...)` (source of truth).
- [x] `src/main.cpp`: update the fallback `MDV_VERSION` used outside CMake builds.
- [x] `CHANGELOG.md`: move the release notes from `Unreleased` to the new version and date.
- [x] `CHANGELOG.ja.md`: make the same release heading change in Japanese.

## Values derived from `PROJECT_VERSION`

- [x] Application `--version`, status bar, and ready status use the compiled `MDV_VERSION`.
- [x] macOS `CFBundleVersion` and `CFBundleShortVersionString` use `PROJECT_VERSION`.
- [x] Linux package filenames and metadata read the version from `CMakeLists.txt`.
- [x] Windows Inno Setup version and installer filename read the version from `CMakeLists.txt`.
- [x] macOS DMG naming reads the version from `CMakeLists.txt` unless explicitly overridden.

## Verification

- [x] Search the repository for the previous version and review every remaining occurrence.
- [x] Build without creating release packages.
- [x] Verify `mdv --version` reports the new version.
- [x] Run the test suite and `git diff --check`.

`qt-default-version.txt` is the Qt dependency version and is not changed for an mdv release.
