# Changelog

All notable changes to this project are listed here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project uses
[semantic versioning](https://semver.org/). Releases are built from the `v<version>` tags.

## [Unreleased]

### Added
- Official firmware updates (`.UPD`) on the stick are listed with their version, labelled *(live usb)*, and run
  live without being installed: the update's `EP085_Main_R` and GUI resources are decrypted out of it into RAM with
  the unit's own key, and its settings are kept on the stick (a copy of the unit's, made once).
- `bootmenu --upd FILE --extract PATH`: decrypts one file of an update's image (only the sectors it covers).

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
