# Using the boot menu

## The stick

```
<stick>/
  autoexec.bin        the boot menu
  myapp               a community program (listed as "myapp")
  myapp.ini           optional settings for it (docs/apps.md)
  TORAIZSP16.UPD      an official firmware update, run live (below)
  apps/other/...      more programs, one folder each
  bootmenu.conf       optional menu settings
  bootmenu.log        written by the menu: what it found, what it started, and any error
```

Each row has a label: **INSTALLED · PERSISTENT** for the official software on the unit, **LIVE USB** for programs
running from the stick, **LAST** for the default.

The official software is always the first row, with the installed firmware version.

## Official firmware, live from the stick

Copy an official SP-16 update file (`TORAIZSP16.UPD`, as downloaded from Pioneer DJ) to the stick, in its root or in
`apps/`. The menu lists it right after the installed software as **Pioneer DJ - Toraiz SP-16 1.60 (live usb)**: the
version is read from the file itself, and the row says whether it is the same as the installed one. Several update
files can sit side by side (one per version, the file name does not matter).

Starting it does **not** update the unit. The menu decrypts only the official application and its screen resources
out of the update file, with the unit's own key, into RAM (about 21 MB; on the stick in `bootmenu-cache/` when RAM is
short), and runs them in place of the installed software. The update's installer never runs and nothing is written
to the unit's internal memory:

- the update's `EP085_Main_R` runs from `/root/pdj` with the unit's libraries, its own GUI resources and its own
  revision (`app.rev`);
- its settings (`/root/data`) are on the stick in `firmware-<version>-data/`, a copy of the unit's own made the first
  time (calibration, preferences), so the installed firmware's settings never change;
- your projects and samples are the usual ones (the unit's internal storage and the stick), as with the installed
  software. A newer firmware may save projects in a format an older installed one cannot open: keep a copy before
  saving over a project with a different version.

When it quits, the menu comes back; pulling the stick brings back the installed software. The first start takes a few
seconds more (unpacking); later starts in the same session reuse the unpacked copy.

A `TORAIZSP16.ini` beside the update file (same name, `.ini`) accepts `name=`, `description=`, `hidden=1`, `accent=`
and `data=` (`data=none` runs it with the unit's own `/root/data` instead of a copy on the stick).

## bootmenu.conf

```
default=last        # last | none | official | an app id (id@stick, id@embedded)
timeout=8           # seconds before the default starts; 0 = wait for you
scan_root=1         # 0 = only look in apps/
show_official=1     # 0 = hide the official software row
official_wait_ms=8000
```

## When things happen

- **An app quits:** the menu comes back.
- **An app fails at once:** the menu comes back and says so; details are in `bootmenu.log`.
- **The stick is pulled:** the app or the menu stops and the official software starts again.
- **The screen cannot be used:** the default entry starts without the menu (or the official software).

## Development images: SSH

An image built with `--ssh-dropbear` and `--ssh-keys` (see [development.md](development.md)) also starts an SSH
server on the LINK port at `169.254.16.16` (key login only). `ctl help` lists the remote commands (`ctl menu`,
`ctl run ID`, `ctl update ID < file`, `ctl log`). Release images have no SSH server.
