#!/bin/sh
# build.sh - cross-compile the boot menu for the unit (armel, Cortex-A9, glibc 2.13) in the toolchain container.
# Output: out/bootmenu (stripped), checked against glibc 2.13 and for GL/X11 libraries in DT_NEEDED (the GPU driver
# and libX11 are loaded at run time).
#
#   tools/build.sh
#
# Environment: TOOLCHAIN_IMAGE (default bootmenu-toolchain:1, built from toolchain/ when missing),
# BM_VERSION (default: the VERSION file, plus "-dev+<commit>" when HEAD is not the matching v<VERSION> tag).
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
cd "$here"
image=${TOOLCHAIN_IMAGE:-bootmenu-toolchain:1}
if ! docker image inspect "$image" >/dev/null 2>&1; then
  echo "building the toolchain image $image (a few minutes, once)"
  docker build -t "$image" toolchain
fi
version=$(cat VERSION)
if [ -z "${BM_VERSION:-}" ]; then
  BM_VERSION=$version
  if git rev-parse --git-dir >/dev/null 2>&1 && [ "$(git describe --tags --exact-match 2>/dev/null)" != "v$version" ]; then
    BM_VERSION="$version-dev+$(git rev-parse --short HEAD 2>/dev/null || echo unknown)"
  fi
fi
[ -f src/gen/font_atlas.h ] || python3 tools/make-font-atlas.py
mkdir -p out
echo "version $BM_VERSION"
docker run --rm --user "$(id -u):$(id -g)" -e BM_VERSION="$BM_VERSION" -v "$here":/work -w /work "$image" sh -c '
  set -e
  arm-linux-gnueabi-gcc -std=gnu99 -O2 -Wall -Wextra -Wshadow -Wno-format-truncation -Wno-restrict -Werror \
    -march=armv7-a -mtune=cortex-a9 -mfpu=neon -mfloat-abi=softfp \
    -fno-pie -no-pie -ffunction-sections -fdata-sections -Wl,--gc-sections -static-libgcc \
    --sysroot=/opt/sp16/sysroot \
    -Wl,-rpath-link=/opt/sp16/sysroot/lib/arm-linux-gnueabi:/opt/sp16/sysroot/usr/lib/arm-linux-gnueabi \
    -DBM_VERSION="\"$BM_VERSION\"" \
    -o out/bootmenu src/*.c -Wl,--no-as-needed -lpthread -Wl,--as-needed -ldl -lrt -lm
  arm-linux-gnueabi-strip out/bootmenu
  file out/bootmenu
  sp16-check-glibc out/bootmenu
  if arm-linux-gnueabi-readelf -d out/bootmenu | grep -E "NEEDED.*(EGL|GLES|GAL|X11)"; then
    echo "error: the menu must load the GPU driver and libX11 at run time" >&2; exit 1
  fi
'
ls -l out/bootmenu
