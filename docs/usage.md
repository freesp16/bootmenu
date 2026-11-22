# Using the boot menu

## The stick

```
<stick>/
  autoexec.bin        the boot menu
  myapp               a community program (listed as "myapp")
  myapp.ini           optional settings for it (docs/apps.md)
  other.enc           a program encrypted with the unit's key (docs/apps.md)
  EP085_Main_R        a copy of the official application, under any name, plain or encrypted (docs/apps.md)
  EP085_Main_R-gui/   optional screen resources for it, e.g. from a modified update (docs/apps.md)
  apps/other/...      more programs, one folder each
  bootmenu.conf       optional menu settings
  bootmenu.log        written by the menu: what it found, what it started, and any error
```

Each row has a label: **INSTALLED · PERSISTENT** for the official software on the unit, **LIVE USB** for programs
running from the stick, **ENCRYPTED** for those encrypted with the unit's key, **LAST** for the default.

The official software is always the first row, with the installed firmware version.

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
- **The stick is pulled:** the app or the menu stops and the official software starts again. What happened then
  could not be written to the stick, so it is added to `bootmenu.log` the next time the stick goes in.
- **The screen cannot be used:** the default entry starts without the menu (or the official software).

## Development images: SSH

An image built with `--ssh-dropbear` and `--ssh-keys` (see [development.md](development.md)) also starts an SSH
server on the LINK port at `169.254.16.16` (key login only). `ctl help` lists the remote commands (`ctl menu`,
`ctl run ID`, `ctl update ID < file`, `ctl log`). Release images have no SSH server.
