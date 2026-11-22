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

The id is the file name without `.bin`, `.elf` or `.app`.

## How it runs

The menu exits before the program starts, so the program gets the screen (`/dev/fb0`, or X on `:0`), the panel
and the audio to itself, just as the official software has them. It runs as root, with:

- its working directory set to its own folder;
- `/root/data` mapped to `<stick>/<id>-data/` for the program alone, so nothing lands in internal memory;
- the `key=value` lines of `<stick>/<id>.conf` as environment variables `<ID>_<KEY>`;
- `BOOTMENU_STICK`, `BOOTMENU_APP_DIR` and `BOOTMENU_APP_ID` set.

When it exits, the menu comes back.

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
