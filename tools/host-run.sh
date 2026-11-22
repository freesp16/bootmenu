#!/bin/sh
# host-run.sh - development only: build the menu natively and run it on this PC through Mesa (GLES2 over X11).
# The product is the armel build (tools/build.sh); this exists to see the screen and test discovery without a unit.
# Needs a C compiler, Mesa (libEGL/libGLESv2), libX11 and, for HEADLESS=1, xvfb-run.
#
#   tools/host-run.sh STICK_DIR [EMBEDDED_DIR] [bootmenu args...]        a window (keys: arrows, Return, Esc, F5, q)
#   HEADLESS=1 tools/host-run.sh STICK_DIR EMBEDDED_DIR --screenshot out.ppm --exit-after 45 [--keys dddp]
#
# Mesa is forced (glvnd may otherwise pick a vendor driver that cannot do EGL on Xvfb).
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
stick=${1:?usage: tools/host-run.sh STICK_DIR [EMBEDDED_DIR] [args...]}
shift
emb=""
if [ $# -gt 0 ] && [ -d "$1" ]; then emb=$1; shift; fi
mkdir -p "$here/build/host"
${CC:-cc} -std=gnu99 -O2 -g -Wall -Wextra -Wshadow -Wno-format-truncation -Wno-restrict -DBM_DEV_HOST \
  -o "$here/build/host/bootmenu" "$here"/src/*.c -ldl -lm
mesa=/usr/share/glvnd/egl_vendor.d/50_mesa.json
[ -f "$mesa" ] && export __EGL_VENDOR_LIBRARY_FILENAMES=${__EGL_VENDOR_LIBRARY_FILENAMES:-$mesa}
export EGL_PLATFORM=x11
run() { "$here/build/host/bootmenu" --render host --no-official-stop --stick "$stick" ${emb:+--embedded "$emb"} \
  --out "$here/build/host/choice.sh" "$@"; }
if [ "${HEADLESS:-0}" = 1 ]; then
  export LIBGL_ALWAYS_SOFTWARE=1
  xvfb-run -a -s "-screen 0 800x480x24" "$here/build/host/bootmenu" --render host --no-official-stop --stick "$stick" \
    ${emb:+--embedded "$emb"} --out "$here/build/host/choice.sh" "$@"
else
  run "$@"
fi
