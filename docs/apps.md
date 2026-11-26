# Making software appear in the menu

## What the menu runs

A program is listed when it is a 32-bit little-endian ARM executable for the unit's userland: EABI, soft-float
calling convention (**armel**), linked against glibc 2.13 or older. Hard-float (armhf) builds and other
architectures are skipped, and `bootmenu.log` says why.

Put it in one of these places on the stick:

| | |
|---|---|
| `<stick>/<file>` | next to `autoexec.bin` |
| `<stick>/apps/<file>` | in the apps folder |
| `<stick>/apps/<id>/` | a folder app: an executable named `<id>`, or `exe=` in `app.ini` |

The id is the file name without `.bin`, `.elf`, `.app` or `.enc`. The file name is free: the menu recognises programs
by their content, not their name.

## Encrypted programs

A program can also be copied to the stick encrypted with the unit's own key, the cipher of `autoexec.bin`:

```bash
tools/encrypt-app.py --key aes256.key myapp --out myapp.enc     # or BOOTMENU_AES_KEY=... tools/encrypt-app.py myapp
```

The menu lists it like a plain one, under its file name (or the name in its app-info note), with an **ENCRYPTED**
label. It decrypts the file with the key on the unit (`/usr/local/pdj/aes256.key`, the one the unit's USB hook uses)
into RAM when it starts; nothing decrypted is written to the stick. The format is the program zero-padded to a multiple
of 512 bytes, AES-256-CBC per 512-byte sector (IV = the little-endian sector number and 12 zero bytes), no header: a
file is an encrypted program when its first sector decrypts to an ELF header.

## The official application from the stick

A copy of the official application, `EP085_Main_R` (from the unit or an update), plain or encrypted, can sit on the
stick under any file name: the menu recognises it by its content and shows *Official SP-16 software* with the release
when its size matches one (*unknown version* for a modified one). It runs as the installed one does, in `/root/pdj`,
with the unit's libraries and screen resources, while the installed copy stays untouched. Its settings (`/root/data`)
are `<stick>/<id>-data/`, a copy of the unit's own made the first time (calibration, preferences), so the installed
software's settings never change; `data=none` in its `.ini` gives it the unit's own instead.

A modified or newer application usually needs its own screen resources too: it loads its images and layouts from
`/root/gui/Resource/skin16/`, and the unit only has those of the installed release. Put them in a folder
`<id>-gui/` beside the program (the files as they go under `/root/gui`, e.g. `my163-gui/Resource/skin16/picture/...`).
While the program runs, the menu lays that folder over a RAM copy of the unit's resources and mounts the result on
`/root/gui`, so the folder may hold everything or only what differs; the unit's own resources are never written. The
row then says *+ own GUI*. `gui=` in the `.ini` names another folder (`gui=none`: none at all). This works for any
program, not only the official one.

`tools/extract-upd.py` takes both out of an update file (`.UPD`) with the unit's key:

```bash
tools/extract-upd.py --key aes256.key TORAIZSP16_1.63.3.UPD --name my163 --out /path/to/stick
# -> my163 (the application) and my163-gui/ (all its screen resources, about 2 MB)
#    --encrypt: my163 encrypted with the key; --base OTHER.UPD: only the resources that differ from OTHER.UPD
```

On a DJS-1000 the same applies to its official application, `EP113_Main_R`. The menu knows each unit's releases by
their size, so a copy made for the other unit is listed with the reason it cannot start.

## A mod of the official software

Some projects change the official software without replacing it: a library loaded into the installed application
when it starts. A folder app runs it with `exe=official`:

```
apps/mymod/
  app.ini            exe=official, preload=libmymod.so (below)
  libmymod.so
```

```
name=My mod
exe=official                  # the unit's installed official application
preload=libmymod.so           # loaded first (LD_PRELOAD), copied to RAM before it starts
needs_official=1.22           # optional: the row says it cannot start on another release (told by the file size)
official_md5=0123...cdef      # optional: checked before it starts, for the exact build the library was written for
env=MYMOD_SETTINGS=${STICK}/mymod/settings.txt    # optional, repeatable: ${STICK}, ${APP_DIR}
```

The application runs as the installed one does, in `/root/pdj` with the unit's own `/root/data` (`data=` names a
stick folder instead). On the DJS-1000 it is started in the environment and with the signal settings the unit gave
it at power-on, and the stick is announced to it again, as for the official software itself (see
[usage.md](usage.md#the-djs-1000)). The row shows the installed release and the library's name. When the
official application is missing, is another release, or the library is not on the stick, the row says so and does
not start.

`env=` lines work for any program, not only these.

## How it runs

The menu exits before the program starts, so the program gets the screen (`/dev/fb0`, or X on `:0`), the panel
and the audio to itself, just as the official software has them. It runs as root, with:

- its working directory set to its own folder (`/root/pdj` for the official application);
- `/root/gui` with its own screen resources laid over the unit's, when it has an `<id>-gui/` folder;
- `/root/data` mapped to `<stick>/<id>-data/` for the program alone, so nothing lands in internal memory;
- the `key=value` lines of `<stick>/<id>.conf` as environment variables `<ID>_<KEY>`, and its `env=` lines;
- `BOOTMENU_STICK`, `BOOTMENU_APP_DIR`, `BOOTMENU_APP_ID` and `BOOTMENU_UNIT` (`sp16` or `djs1000`) set.

A program written for both units reads `BOOTMENU_UNIT` to tell them apart. Their panels report the same buttons in
different places of the frame (see [usage.md](usage.md#the-djs-1000)). On the DJS-1000 the panel holds the outputs
muted until a program clears that request in the frames it sends.

Programs from the stick run from a RAM disk of their own (the unit's `/tmp` is small), so they do not depend on the
stick while they run. When it exits, the menu comes back.

## Settings: `<id>.ini` or `app.ini`

```
name=My App
description=What it does
args=--fullscreen
data=myapp-data        # none = no /root/data redirect
conf=myapp.conf
prefix=MYAPP_
run_from=ram           # ram (copied to /tmp first) or stick
accent=#4f8cff
hidden=1
gui=my-resources       # files for /root/gui (default: <id>-gui/ when it exists; none = never)
env=KEY=value          # an environment variable (repeatable), ${STICK} and ${APP_DIR} replaced
exe=official           # folder apps: the installed official application (see above), with preload=,
                       # needs_official= and official_md5=
```

All keys are optional.

## Name, version and icon inside the program

A program can carry an ELF note that the menu reads without running it: section `.note.appinfo`, owner `APPINFO`,
type 1. The descriptor is little-endian:

| offset | field |
|---|---|
| 0 | `"AINF"` |
| 4 | u16 format = 1 |
| 6 | u16 header size = 128 |
| 8 | char[48] name (UTF-8, NUL-padded) |
| 56 | char[24] version |
| 80 | char[40] id (reverse DNS) |
| 120 | u16 width, u16 height: a square power of two, 16 to 256 (128 is a good size) |
| 124 | u32 pixel format = 1 (RGBA8, straight alpha, rows top to bottom) |
| 128 | the pixels |

In C, one `const` struct with `__attribute__((section(".note.appinfo"), aligned(4), used))` is enough: the
assembler marks it as a note, the linker gives it a `PT_NOTE` segment, and `strip` keeps it.
`tools/test-arm.sh` has a small example. A `name=` in the `.ini` file wins over the note.

### Standby handoff

For an app with a data directory on the boot volume, the launcher sets
`BOOTMENU_STANDBY_READY` to a fresh receipt path in RAM. An app requesting
standby should write the receipt after completing its saves, close its
devices and exit with status 76. The launcher requires both the receipt
and the exit status. It releases the app's storage binds, closes the USB
log, syncs, and unmounts the boot volume before signaling the panel.

An empty receipt path means the launcher cannot offer this handoff. If
the launcher cannot release storage, it leaves the unit powered on and
reports the error in the menu.
