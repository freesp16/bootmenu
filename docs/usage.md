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
  apps/mymod/         the official software with a library of its own loaded first (docs/apps.md)
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
- **The screen cannot be used:** the default entry starts without the menu (or the official software). An entry
  that cannot start on the unit is never the default.

## The DJS-1000

The same `autoexec.bin` runs on the SP-16 and the DJS-1000: the two share the board, the screen and the key. The
menu tells them apart by the official application installed (`EP085_Main_R` or `EP113_Main_R`) and shows the model
in its header. The controls are the same: the rotary selector or the first knob under the screen chooses, its push
or PLAY starts, BACK and HOME work as above. The DJS-1000's power key works in the menu too: it puts the unit in
standby as the official software does.

What differs on the DJS-1000:

- **The official software is started as the unit starts it.** Before the menu stops it, the environment and the
  signal settings the unit gave it are read from `/proc`. When it starts again (BACK twice, or pulling the stick), it
  gets them back, and the stick is announced to it again. Started from the USB hook as it is, it would run in udev's
  and not see the stick.
- **With the stick in at power-on**, the menu waits for the unit to start its official software (90 s at most) and
  then stops it, so the two never draw over each other.
- **The panel.** The menu turns every pad and button lamp off and keeps the outputs muted while it is shown. A
  program started from it clears that request itself once its audio runs. The link keeps running when the menu
  exits, as the official software expects it.
- **Touch** uses the SP-16's mapping until it is checked on a unit. Each touch's raw values are in `bootmenu.log`.

`--unit sp16` or `--unit djs1000` (or `BOOTMENU_UNIT`) overrides the detection, for development.

## Development images: SSH

An image built with `--ssh-dropbear` and `--ssh-keys` (see [development.md](development.md)) also starts an SSH
server on the LINK port at `169.254.16.16` (key login only). `ctl help` lists the remote commands (`ctl menu`,
`ctl run ID`, `ctl update ID < file`, `ctl log`). Release images have no SSH server.
