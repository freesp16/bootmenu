# Changelog

All notable changes to this project are listed here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project uses
[semantic versioning](https://semver.org/). Releases are built from the `v<version>` tags.

## [Unreleased]

### Added
- Programs encrypted with the unit's key (the cipher of `autoexec.bin`) are listed under their file name with an
  ENCRYPTED label and decrypted into RAM when started; `tools/encrypt-app.py` makes them, `bootmenu --decrypt`
  decrypts one.
- The official application (`EP085_Main_R`) on the stick, plain or encrypted and under any file name, is recognised
  by its content and runs as the installed one does (in `/root/pdj`), with a copy of the unit's settings on the stick.
- A program's own screen resources: an `<id>-gui/` folder beside it (or `gui=`) is laid over a RAM copy of the
  unit's `/root/gui` while it runs, so a modified official application shows its new images. `tools/extract-upd.py`
  takes the application and its resources out of an update file.

### Fixed
- Pulling the stick always brings back the official software: the stick watcher stops the program, undoes the mounts
  and starts the official software itself, instead of waiting for the runner, which could stay stuck behind a program
  that did not end. Each bind mount is undone exactly once.

### Changed
- What happens after the stick is pulled is logged in RAM and added to `bootmenu.log` at the next insertion.
- Programs from the stick run from a RAM disk of their own instead of `/tmp`, which is too small on the unit for
  larger programs (they ran from the stick instead).

## [1.0.0] - 2026-11-22

First public release.

### Added
- Boot menu on the unit's screen (OpenGL ES 2), started from a USB stick through the unit's own autoexec hook.
- Lists the official software (always first, with its firmware version) and every compatible armel program on
  the stick, beside `autoexec.bin` or in `apps/`.
- Shows an app's own name, version and icon when the program carries an app-info note.
- SELECT encoder, push, PLAY, BACK, HOME and the touch screen; instant selection.
- Remembers the last choice and starts it after a countdown (configurable in `bootmenu.conf`).
- Returns to the menu when an app quits, and shows why when it fails at start.
- Pulling the stick brings the official software back at once.
- Apps keep their data on the stick: nothing is written to the unit's internal memory.
