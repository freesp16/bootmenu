# Changelog

All notable changes to this project are listed here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project uses
[semantic versioning](https://semver.org/). Releases are built from the `v<version>` tags.

## [Unreleased]

### Added
- The DJS-1000: the same `autoexec.bin` runs on it, tells the unit by its installed official application and shows
  the model in the header. Its panel layout and POWER key are read, and its lamps are off while the menu is shown.
  Its official software is started again in the environment and with the signal settings the unit gave it, with the
  stick announced to it again, and the menu waits for it when the stick is in at power-on.
- Mods of the official software: a folder app with `exe=official` starts the installed official application with
  a library loaded first (`preload=`), optionally only on one release (`needs_official=`) or build
  (`official_md5=`). A row that cannot start on the unit says why.
- `env=` lines in an app's settings, with `${STICK}` and `${APP_DIR}`, and `BOOTMENU_UNIT` for every program.
- A screen drawn by the CPU (`--render cpu`) when the GPU cannot draw the menu. On the DJS-1000
  it comes right after the framebuffer, since the GPU's X11 window stays black there.

### Fixed
- The X server's cursor no longer shows over the menu or the programs it starts.
- When the menu's last screen fallback crashes instead of failing, the default entry still starts without a screen.
- Ejecting the stick from the official software no longer waits about 10 s: the session keeps nothing open on it.
  The official software's own output is no longer written to `bootmenu.log`.

## [1.1.0] - 2026-11-24

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
- The screen is bright again when the menu starts after the official software's screen saver dimmed it: once the
  official software is gone, the menu sets the backlight to its LCD BRIGHTNESS setting, as the official software
  does when it starts. Apps started with `ctl run` get the same (`bootmenu --wake-backlight`).

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
