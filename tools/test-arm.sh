#!/bin/sh
# test-arm.sh - run the armel build (out/bootmenu) under qemu-arm (Cortex-A9) against a fake stick: app discovery,
# the ELF compatibility check, the app-info note (name, version, icon), programs encrypted with the unit's key (made
# here with a throwaway key by tools/encrypt-app.py), the official application under another file name, --decrypt,
# --resolve and the choice file. No GPU here, so the screen itself is not tested (tools/host-run.sh shows it on a PC).
#
#   tools/build.sh && tools/test-arm.sh          TOOLCHAIN_IMAGE as for build.sh; needs python3-cryptography
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
image=${TOOLCHAIN_IMAGE:-bootmenu-toolchain:1}
[ -f "$here/out/bootmenu" ] || { echo "no out/bootmenu: run tools/build.sh first" >&2; exit 1; }
t=$(mktemp -d "${TMPDIR:-/tmp}/bootmenu-arm.XXXXXX")
trap 'rm -rf "$t"' EXIT
mkdir -p "$t/stick/apps/demo" "$t/stick/apps/plain" "$t/progs"

# 1) the test programs, built in the container
docker run --rm --user "$(id -u):$(id -g)" -v "$t":/t "$image" sh -eu -c '
  cat > /t/demo.c <<C
#include <stdint.h>
struct Note { uint32_t namesz, descsz, type; char owner[8]; char magic[4]; uint16_t format, header;
              char name[48], version[24], id[40]; uint16_t w, h; uint32_t pixel_format; uint8_t px[16 * 16 * 4]; };
__attribute__((section(".note.appinfo"), aligned(4), used)) const struct Note note = {
    8, sizeof(struct Note) - 20, 1, "APPINFO", {65, 73, 78, 70}, 1, 128, "Demo App", "2.3.4", "org.example.demo", 16, 16, 1, {0}};
int main(void) { return 0; }
C
  cc="arm-linux-gnueabi-gcc -march=armv7-a -mfloat-abi=softfp -fno-pie -no-pie --sysroot=/opt/sp16/sysroot"
  $cc -o /t/stick/apps/demo/demo /t/demo.c
  echo "int main(void) { return 0; }" > /t/plain.c && $cc -o /t/stick/apps/plain/plain /t/plain.c
  $cc -o /t/progs/demo /t/demo.c
  # a stand-in for the official application: built with the RPATH of its build tree
  $cc -Wl,--disable-new-dtags -Wl,-rpath,/usr/X11R6/lib/:/home/iMX6/ltib/rootfs/usr/lib/ -o /t/progs/official /t/plain.c
  cc -o /t/stick/native /t/plain.c              # an x86-64 program: must be skipped'

# 2) encrypted with a throwaway key on the host
echo "throwaway-test-key-not-for-any-unit" > "$t/test.key"
echo "another-throwaway-key" > "$t/wrong.key"
enc() { python3 "$here/tools/encrypt-app.py" --key "$t/test.key" "$1" --out "$2" >/dev/null; }
enc "$t/progs/demo" "$t/stick/secret.enc"               # an encrypted app with an app-info note
cp "$t/progs/official" "$t/stick/EP085_Main_R"           # the official application, plain
enc "$t/progs/official" "$t/stick/renamed_fw"            # ...and encrypted under another name
mkdir -p "$t/stick/renamed_fw-gui/Resource" "$t/stick/apps/stray-gui"   # its screen resources; a -gui folder in apps/
head -c 4096 /dev/urandom > "$t/stick/RANDOM.REC"        # not a program, whole sectors: must be ignored quietly
echo "description=no app info" > "$t/stick/apps/plain/app.ini"
echo "not a program" > "$t/stick/notes.txt"
echo "default=demo" > "$t/stick/bootmenu.conf"

# 3) the tests
docker run --rm --user "$(id -u):$(id -g)" -v "$here":/work:ro -v "$t":/t "$image" sh -eu -c '
  t=/t
  run() { QEMU_CPU=cortex-a9 qemu-arm -L /opt/sp16/sysroot /work/out/bootmenu "$@"; }
  fail=0
  check() { if grep -q "$2" "$3"; then echo "PASS $1"; else echo "FAIL $1"; cat "$3"; fail=1; fi; }
  run --stick $t/stick --key $t/test.key --list > $t/list 2> $t/log
  check "the official software is listed first" "^official@official" $t/list
  check "an armel app is listed" "^plain@stick" $t/list
  check "the app-info name replaces the id" "^demo@stick	Demo App" $t/list
  check "the app-info version and icon are read" "app info \"Demo App\" 2.3.4, icon 16x16" $t/log
  check "an x86-64 program is skipped with a reason" "native: not 32-bit little-endian ARM" $t/log
  if grep -q "notes.txt\|RANDOM.REC" $t/list $t/log; then echo "FAIL a data file was considered"; fail=1; else echo "PASS other files are ignored"; fi
  check "an encrypted program is listed under its file name" "^secret@stick	Demo App	$t/stick/secret.enc" $t/list
  check "its app-info note is read through the key" "secret.enc: app info \"Demo App\" 2.3.4, icon 16x16 (encrypted)" $t/log
  check "the official application is recognised by content" "EP085_Main_R: the official SP-16 application, unknown version" $t/log
  check "...under any name, encrypted too" "renamed_fw: the official SP-16 application (encrypted)" $t/log
  check "...and listed under its file name" "^renamed_fw@stick	renamed_fw	" $t/list
  run --stick $t/stick --key $t/test.key --resolve renamed_fw --out $t/fw.sh 2>/dev/null
  check "it runs in /root/pdj" "^BM_DIR=./root/pdj.$" $t/fw.sh
  check "the choice says encrypted" "^BM_ENCRYPTED=.1.$" $t/fw.sh
  check "the choice says official application" "^BM_OFFICIAL_BUILD=.1.$" $t/fw.sh
  check "its <name>-gui folder is its screen resources" "^BM_GUI=.$t/stick/renamed_fw-gui.$" $t/fw.sh
  check "...and the row says so" "renamed_fw: screen resources from $t/stick/renamed_fw-gui" $t/log
  if grep -q "stray-gui" $t/list $t/log; then echo "FAIL a -gui folder was taken for an app"; fail=1; else echo "PASS -gui folders are not apps"; fi
  run --stick $t/stick --key $t/test.key --resolve plain --out $t/plain.sh 2>/dev/null
  check "a plain app is not encrypted" "^BM_ENCRYPTED=.0.$" $t/plain.sh
  check "an app without a -gui folder keeps the unit'"'"'s resources" "^BM_GUI=..$" $t/plain.sh
  run --key $t/test.key --decrypt $t/stick/renamed_fw > $t/dec 2>/dev/null
  n=$(wc -c < $t/progs/official)
  if head -c $n $t/dec | cmp -s - $t/progs/official; then echo "PASS the ARM build decrypts a program exactly"
  else echo "FAIL --decrypt output differs"; fail=1; fi
  run --stick $t/stick --key $t/wrong.key --list > $t/list2 2>/dev/null
  if grep -q "secret\|renamed_fw" $t/list2; then echo "FAIL listed with a wrong key"; fail=1; else echo "PASS a wrong key lists no encrypted program"; fi
  run --stick $t/stick --key $t/missing.key --list > /dev/null 2> $t/log3
  check "without a key file encrypted programs are left out, and the log says so" "no key at $t/missing.key" $t/log3
  rc=0; run --key $t/missing.key --decrypt $t/stick/renamed_fw > /dev/null 2>&1 || rc=$?
  [ $rc -eq 7 ] && echo "PASS --decrypt without a key fails" || { echo "FAIL --decrypt without a key: rc=$rc"; fail=1; }
  run --stick $t/stick --key $t/test.key --resolve default --out $t/choice.sh 2>/dev/null
  check "--resolve default writes the configured default" "^BM_ID=.demo.$" $t/choice.sh
  run --stick $t/stick --resolve stock --out $t/choice2.sh 2>/dev/null
  check "the old name stock still resolves to the official software" "^BM_KIND=.official.$" $t/choice2.sh
  run --version > $t/version
  check "--version prints the build version" "^bootmenu [0-9]" $t/version
  exit $fail'
