# Development

## Layout

| | |
|---|---|
| `src/` | the menu, C99: start-up and loop (`main.c`), OpenGL ES (`gfx.c`), the screen (`ui.c`), app discovery (`apps.c`), encrypted programs (`crypt.c`), panel input (`panel.c`) |
| `image/` | the scripts inside `autoexec.bin`: `autoexec.sh` (logs, then runs `launcher.sh`) and `launcher.sh` (runs the menu, then the chosen app) |
| `toolchain/` | the build container: Debian's armel cross GCC with a glibc 2.13 sysroot |
| `tools/` | build, image, tests |
| `fonts/` | Inter subsets, baked into `src/gen/font_atlas.h` by `tools/make-font-atlas.py` (needs Pillow) |

## Build and test

```bash
tools/build.sh            # out/bootmenu; builds the toolchain image the first time (a few minutes)
tools/test-arm.sh         # the armel build under qemu: discovery, compatibility check, app-info note, encrypted programs, choices
tools/test-launcher.sh    # the scripts end to end in a container standing in for the unit (needs busybox-static)
tools/host-run.sh STICK   # a native build in a window on a Linux PC (Mesa), to see the screen
```

Both tests encrypt their test programs with a throwaway key (`tools/encrypt-app.py`, needs `python3-cryptography`).

The version comes from `VERSION`. Builds that are not on the matching tag report `<version>-dev+<commit>`.

## The image

```bash
BOOTMENU_AES_KEY='<the key line>' tools/build-image.py        # out/autoexec.bin
tools/build-image.py --key key.file --app path/to/app          # embed an app (optional)
tools/build-image.py --key key.file --ssh-dropbear dropbearmulti --ssh-keys ~/.ssh/id_ed25519.pub   # dev image
```

The device key is never committed (`*.key` is ignored). The key can come from `--key FILE`, `BOOTMENU_AES_KEY`
(the line itself) or `BOOTMENU_AES_KEY_FILE`.

## CI and releases

- **CI** (`.github/workflows/ci.yml`, every push and pull request): shell and Python checks, the armel build with
  the glibc 2.13 check, the qemu tests, the launcher tests, and an image packaging check with a throwaway key.
  The built binary is kept as an artifact.
- **Release** (`.github/workflows/release.yml`, on a `v*` tag): checks that the tag matches `VERSION` and that
  `CHANGELOG.md` has that section, builds and tests, encrypts `autoexec.bin` with the `BOOTMENU_AES_KEY` secret,
  and publishes a GitHub release with `autoexec.bin`, `bootmenu-armel`, `SHA256SUMS` and the changelog section
  as notes.

One-time setup: in the repository settings, create the environment **release** and add the secret
**BOOTMENU_AES_KEY** (the first line of the device key file).

To release:

1. Move the `Unreleased` entries in `CHANGELOG.md` under a new `## [X.Y.Z] - date` heading.
2. Write `X.Y.Z` to `VERSION` and commit.
3. `git tag vX.Y.Z && git push origin main vX.Y.Z`
