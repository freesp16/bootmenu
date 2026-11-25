# Changelog

All notable changes to this project are listed here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project uses
[semantic versioning](https://semver.org/). Releases are built from the `v<version>` tags.

## [Unreleased]

### Added
- Physical POWER button handling with Sub-MCU Standby ACK handshake: pressing POWER/STANDBY exits the menu with
  `rc=9` and halts cleanly without restarting the official software.
- Rescue safe mode flag on the USB stick: creating `SP16_SAFE_MODE` or `bootmenu.disable` at the root of the stick
  bypasses the boot menu and leaves the official software untouched.
- Configurable cold-boot wait (`cold_boot_wait_s=25`, `0 = off`) in `bootmenu.conf` to wait for stock `apl_start`
  before taking over, returning immediately on hot-plug.
- Optional resistive touchscreen calibration override (`touch_cal=rawLeft,rawRight,rawTop,rawBottom`) in
  `bootmenu.conf`.
- Static **armhf** executables (with no dynamic interpreter, e.g. `musl -static`) alongside armel, and explicit POSIX
  `#!/bin/sh` launcher scripts via `exe=` in folder `app.ini` (defaulting `run_from=stick`; stray `.sh` files outside
  `app.ini` remain ignored).
- Opt-in `autostart=1` in `bootmenu.conf` to launch the default entry directly without showing the menu, returning to
  the menu when the app exits or if no default entry resolves.
- Opt-in read-only HTTP status bridge (`http=1`, `http_port=8080` in `bootmenu.conf`, off by default) via BusyBox
  `httpd` serving a relative-URL status/log/framebuffer viewer (`index.html`, `/cgi-bin/status.cgi`,
  `/cgi-bin/log.cgi`, `/cgi-bin/fb0.cgi`) and keeping `eth0:16` up even when `ssh=0`.

### Fixed
- Fall back to `--resolve default` (or `official` when an error message is active) when `gl-x11` crashes without
  producing a choice.
- Stage `ctl update` uploads and unpacked embedded apps in the 128 MB `$STAGE/run` RAM disk instead of the small
  `/tmp` filesystem.
- Bind Dropbear explicitly to IPv4 (`0.0.0.0:<port>` when no address is given), scope SSH teardown to
  `$STAGE/dropbear.pid`, and match `/proc/mounts` paths literally with `grep -qF`.

### Changed
- `launch_app()` executes `BM_SCRIPT=1` folder-app scripts under `/bin/sh` in both mount-namespace (`unshare -m`) and
  global bind-mount modes.

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
