#!/bin/sh
# test-arm.sh - run the armel build (out/bootmenu) under qemu-arm (Cortex-A9) against a fake stick: app discovery,
# the ELF compatibility check, the app-info note (name, version, icon), --resolve and the choice file. No GPU here,
# so the screen itself is not tested (tools/host-run.sh shows it on a PC).
#
#   tools/build.sh && tools/test-arm.sh          TOOLCHAIN_IMAGE as for build.sh
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
image=${TOOLCHAIN_IMAGE:-bootmenu-toolchain:1}
[ -f "$here/out/bootmenu" ] || { echo "no out/bootmenu: run tools/build.sh first" >&2; exit 1; }
docker run --rm --user "$(id -u):$(id -g)" -v "$here":/work:ro "$image" sh -eu -c '
  t=$(mktemp -d); mkdir -p $t/stick/apps/demo $t/stick/apps/plain
  cat > $t/demo.c <<C
#include <stdint.h>
struct Note { uint32_t namesz, descsz, type; char owner[8]; char magic[4]; uint16_t format, header;
              char name[48], version[24], id[40]; uint16_t w, h; uint32_t pixel_format; uint8_t px[16 * 16 * 4]; };
__attribute__((section(".note.appinfo"), aligned(4), used)) const struct Note note = {
    8, sizeof(struct Note) - 20, 1, "APPINFO", {65, 73, 78, 70}, 1, 128, "Demo App", "2.3.4", "org.example.demo", 16, 16, 1, {0}};
int main(void) { return 0; }
C
  cc="arm-linux-gnueabi-gcc -march=armv7-a -mfloat-abi=softfp -fno-pie -no-pie --sysroot=/opt/sp16/sysroot"
  $cc -o $t/stick/apps/demo/demo $t/demo.c
  echo "int main(void) { return 0; }" > $t/plain.c && $cc -o $t/stick/apps/plain/plain $t/plain.c
  echo "description=no app info" > $t/stick/apps/plain/app.ini
  cc -o $t/stick/native $t/plain.c              # an x86-64 program: must be skipped
  echo "not a program" > $t/stick/notes.txt
  echo "default=demo" > $t/stick/bootmenu.conf
  run() { QEMU_CPU=cortex-a9 qemu-arm -L /opt/sp16/sysroot /work/out/bootmenu "$@"; }
  fail=0
  check() { if grep -q "$2" "$3"; then echo "PASS $1"; else echo "FAIL $1"; cat "$3"; fail=1; fi; }
  run --stick $t/stick --list > $t/list 2> $t/log
  check "the official software is listed first" "^official@official" $t/list
  check "an armel app is listed" "^plain@stick" $t/list
  check "the app-info name replaces the id" "^demo@stick	Demo App" $t/list
  check "the app-info version and icon are read" "app info \"Demo App\" 2.3.4, icon 16x16" $t/log
  check "an x86-64 program is skipped with a reason" "native: not 32-bit little-endian ARM" $t/log
  if grep -q notes.txt $t/list $t/log; then echo "FAIL a text file was considered"; fail=1; else echo "PASS other files are ignored"; fi
  run --stick $t/stick --resolve default --out $t/choice.sh 2>/dev/null
  check "--resolve default writes the configured default" "^BM_ID=.demo.$" $t/choice.sh
  run --stick $t/stick --resolve stock --out $t/choice2.sh 2>/dev/null
  check "the old name stock still resolves to the official software" "^BM_KIND=.official.$" $t/choice2.sh
  run --version > $t/version
  check "--version prints the build version" "^bootmenu [0-9]" $t/version
  exit $fail'
