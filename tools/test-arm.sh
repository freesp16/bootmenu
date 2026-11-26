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

# --exec's probe: what a program started through it is given (environment, blocked and ignored signals)
cat > "$t/probe.c" <<'C'
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
static const char *v(const char *k) { const char *s = getenv(k); return s ? s : "-"; }
int main(void) {
    sigset_t set;
    struct sigaction sa;
    sigprocmask(SIG_BLOCK, NULL, &set);
    sigaction(SIGHUP, NULL, &sa);
    printf("FROM_FILE=%s KEPT=%s KEPT_P=%s DROPPED=%s EXEC_VARS=%s\n", v("FROM_FILE"), v("KEPT"), v("KEPT_PREFIX_X"),
           v("DROPPED"), v("BOOTMENU_EXEC_PRELOAD"));
    printf("LD_PRELOAD=%s\n", v("LD_PRELOAD"));
    printf("USR1_BLOCKED=%d HUP_IGNORED=%d\n", sigismember(&set, SIGUSR1), sa.sa_handler == SIG_IGN);
    return 0;
}
C

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
  $cc -o /t/progs/probe /t/probe.c
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
# the official application's settings file (header, then 0x8c-byte records: id at +0, value at +0x88): LCD BRIGHTNESS
# (id 5) at 5, and a copy with it out of range
settings() { python3 -c 'import struct, sys
rec = lambda i, v: struct.pack("<i", i) + bytes(0x84) + struct.pack("<i", v)
open(sys.argv[1], "wb").write(bytes.fromhex("8f9e7b2a04000100") + rec(3, 2) + rec(5, int(sys.argv[2])) + rec(6, 32))' "$@"; }
settings "$t/settings.dat" 5
settings "$t/settings-bad.dat" 9
# A mod of the official software: the installed application with a library preloaded (not installed here)
mkdir -p "$t/stick/apps/mods"
printf 'name=Some mods\nexe=official\npreload=libmods.so\nneeds_official=1.22\nenv=MODS_CFG=${STICK}/mods/mods.txt\nenv=MODS_DIR=${APP_DIR}\n' \
  > "$t/stick/apps/mods/app.ini"
echo "not really a library" > "$t/stick/apps/mods/libmods.so"
mkdir -p "$t/stick2" && echo "default=mods" > "$t/stick2/bootmenu.conf" && cp -R "$t/stick/apps" "$t/stick2/"
# --exec's environment file and signal settings (SIGUSR1 = 10 blocked, SIGHUP = 1 ignored)
printf 'FROM_FILE=yes\nLD_PRELOAD=/from/the/file.so\nDROPPED=no\n' > "$t/official.env"

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
  check "the official application is recognised by content" "EP085_Main_R: the official (model unknown) application, unknown version" $t/log
  check "...under any name, encrypted too" "renamed_fw: the official (model unknown) application (encrypted)" $t/log
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
  rc=0; run --wake-backlight --settings $t/settings.dat 2> $t/bl || rc=$?
  check "the backlight follows the official LCD BRIGHTNESS" "LCD BRIGHTNESS 5 -> level 248" $t/bl
  [ $rc -eq 8 ] && echo "PASS --wake-backlight without /dev/backlight fails" || { echo "FAIL --wake-backlight: rc=$rc"; fail=1; }
  run --wake-backlight --settings $t/settings-bad.dat 2> $t/bl2 || true
  check "an out-of-range LCD BRIGHTNESS means the default" "LCD BRIGHTNESS 3 -> level 91" $t/bl2
  run --wake-backlight --settings $t/missing.dat 2> $t/bl3 || true
  check "no settings file means the default" "LCD BRIGHTNESS 3 -> level 91" $t/bl3
  run --version > $t/version
  check "--version prints the build version" "^bootmenu [0-9]" $t/version

  # the DJS-1000 (neither official application is installed here, so the unit is named)
  run --unit djs1000 --unit-info > $t/unit
  check "--unit-info names the DJS-1000" "^UNIT_KEY=.djs1000.$" $t/unit
  check "...and its official application" "^OFFICIAL_COMM=.EP113_Main_R.$" $t/unit
  run --unit-info > $t/unit0
  check "without either application the unit is the SP-16, as before" "^OFFICIAL_COMM=.EP085_Main_R.$" $t/unit0
  BOOTMENU_UNIT=djs1000 run --unit-info > $t/unit2
  check "BOOTMENU_UNIT names it too" "^UNIT_KEY=.djs1000.$" $t/unit2
  run --unit djs1000 --stick $t/stick --key $t/test.key --list > $t/dlist 2> $t/dlog
  check "the official row is the DJS-1000 software" "^official@official	Pioneer DJ - DJS-1000	/root/pdj/EP113_Main_R" $t/dlist
  check "the log says which unit" "unit=djs1000\|official: /root/pdj/EP113_Main_R not found" $t/dlog
  check "an exe=official entry is listed" "^mods@stick	Some mods	/root/pdj/EP113_Main_R" $t/dlist
  check "...and says why it cannot start here" "mods: the official DJS-1000 application with .*libmods.so, 2 environment line(s): Needs the official DJS-1000 software, which is not installed" $t/dlog
  run --unit djs1000 --stick $t/stick --key $t/test.key --resolve mods --out $t/mods.sh 2>/dev/null
  check "its choice runs the official application" "^BM_AS_OFFICIAL=.1.$" $t/mods.sh
  check "...with its library" "^BM_PRELOAD=.$t/stick/apps/mods/libmods.so.$" $t/mods.sh
  check "...its environment lines" "^BM_ENV_1=.MODS_CFG=\${STICK}/mods/mods.txt.$" $t/mods.sh
  check "...and keeps the settings of the unit" "^BM_DATA=.none.$" $t/mods.sh
  check "the choice names the unit" "^BM_UNIT=.djs1000.$" $t/mods.sh
  run --unit djs1000 --stick $t/stick2 --resolve default --out $t/def.sh 2>/dev/null
  check "a default that cannot start is never chosen without a screen" "^BM_KIND=.official.$" $t/def.sh

  # --exec: the environment of a file, names kept over it, a preload, signal settings. The program it starts is an
  # ARM one too: the host runs it through binfmt (qemu), told where the sysroot is by QEMU_LD_PREFIX (kept).
  QEMU_LD_PREFIX=/opt/sp16/sysroot BOOTMENU_EXEC_ENV=$t/official.env BOOTMENU_EXEC_KEEP="KEPT KEPT_PREFIX_* QEMU_*" \
    BOOTMENU_EXEC_PRELOAD=/ram/libmods.so BOOTMENU_EXEC_SIGNALS="SigBlk: 0000000000000200 SigIgn: 0000000000000001 " \
    KEPT=yes KEPT_PREFIX_X=yes DROPPED=yes LD_PRELOAD=/the/current.so run --exec -- $t/progs/probe > $t/exec 2>&1 || true
  if grep -q "FROM_FILE=" $t/exec; then
    check "--exec starts from the environment file" "FROM_FILE=yes" $t/exec
    check "...keeps the names it is given" "KEPT=yes KEPT_P=yes DROPPED=no EXEC_VARS=-" $t/exec
    check "...puts the preload first, before the current LD_PRELOAD, never the file'"'"'s" "^LD_PRELOAD=/ram/libmods.so:/the/current.so$" $t/exec
    check "...and applies the signal settings" "USR1_BLOCKED=1 HUP_IGNORED=1" $t/exec
  else
    echo "SKIP --exec: this host cannot start an ARM program from one (no binfmt for qemu-arm)"; sed -n 1,3p $t/exec
  fi
  exit $fail'
