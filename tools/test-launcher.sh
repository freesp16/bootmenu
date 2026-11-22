#!/bin/sh
# test-launcher.sh - development only: run image/autoexec.sh end to end in a container standing in for the unit.
#
# BusyBox (a static one from the host, $BUSYBOX) provides the shell and the applets, as on the unit. A fake official app
# (/root/pdj/EP085_Main_R, a shell loop), a fake stick (a folder, so the stick-removal watcher stays quiet until the
# test removes it), the menu built natively with -DBM_DEV_HOST (it lists x86-64 programs), and two test apps:
# `hello` (prints what the launcher gave it, then quits) and `crashy` (fails at once), and with a throwaway key in the
# unit's key path, `secret` (hello encrypted) and `my_fw` (a stand-in for the official application, encrypted, under
# another name). There is no GPU and no X here, so the menu takes its no-screen path: gl-fb fails, gl-x11 fails,
# --fallback picks the default.
#
#   tools/test-launcher.sh          needs Docker, the toolchain image (tools/build.sh builds it) and
#                                   python3-cryptography; no host changes
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
image=${TOOLCHAIN_IMAGE:-bootmenu-toolchain:1}
busybox=${BUSYBOX:-$(command -v busybox || true)}
[ -n "$busybox" ] && file -L "$busybox" | grep -q "statically linked" \
  || { echo "needs a static busybox (apt install busybox-static), or BUSYBOX=path" >&2; exit 1; }
work=$(mktemp -d "${TMPDIR:-/tmp}/bootmenu-test.XXXXXX")
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/iso/apps/hello" "$work/vol/stick" "$work/bin"
cp "$here/image/autoexec.sh" "$here/image/launcher.sh" "$work/iso/"
cp "$busybox" "$work/bin/busybox"

cat > "$work/hello.c" <<'C'
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
int main(int argc, char **argv) {
    char cwd[256];
    printf("HELLO argc=%d arg1=%s cwd=%s HOME=%s HELLO_GREETING=%s BOOTMENU_APP_ID=%s\n", argc, argc > 1 ? argv[1] : "-",
           getcwd(cwd, sizeof cwd), getenv("HOME"), getenv("HELLO_GREETING"), getenv("BOOTMENU_APP_ID"));
    FILE *f = fopen("/root/data/hello/written-by-hello", "w");
    printf("HELLO /root/data writable=%s\n", f ? "yes" : "no");
    if (f) fclose(f);
    fflush(stdout);
    sleep(2);
    return 0;
}
C
printf 'int main(void) { return 3; }\n' > "$work/crashy.c"

# The stand-in official application: where it runs and what /root/data holds for it.
cat > "$work/fakefw.c" <<'C'
#include <stdio.h>
#include <string.h>
#include <unistd.h>
int main(int argc, char **argv) {
    char cwd[256], dat[64] = "";
    FILE *f = fopen("/root/data/Toraiz_SP16.dat", "r");
    if (f) { if (!fgets(dat, sizeof dat, f)) dat[0] = 0; fclose(f); }
    dat[strcspn(dat, "\n")] = 0;
    printf("FAKEFW argv0=%s cwd=%s dat=%s\n", argv[0], getcwd(cwd, sizeof cwd), dat);
    f = fopen("/root/data/written-by-fw", "w");
    if (f) fclose(f);
    fflush(stdout);
    sleep(2);
    return argc > 99;
}
C
echo "throwaway-test-key-not-for-any-unit" > "$work/test.key"

cat > "$work/test.sh" <<'TEST'
set -u
# the unit's key, where its USB autoexec hook reads it (a throwaway one here)
mkdir -p /usr/local/pdj && cp /work/test.key /usr/local/pdj/aes256.key
B=/opt/bb
mkdir -p $B && for a in $(/work/bin/busybox --list); do ln -sf /work/bin/busybox $B/$a; done
export PATH=$B
fail=0
check() { if grep -q "$2" "$3"; then echo "PASS $1"; else echo "FAIL $1 (no '$2' in $3)"; fail=1; fi; }
wait_for() { i=0; while ! grep -q "$1" "$2" 2>/dev/null && [ $i -lt ${3:-30} ]; do sleep 1; i=$((i+1)); done; }

# the unit: a official app, its settings in /root/data
mkdir -p /root/pdj /root/data
echo "unit settings" > /root/data/Toraiz_SP16.dat
printf '#!/bin/sh\ntrap "exit 0" TERM\nwhile :; do sleep 1; done\n' > /root/pdj/EP085_Main_R
chmod 0755 /root/pdj/EP085_Main_R
( cd /root/pdj && ./EP085_Main_R & )
sleep 1; pidof EP085_Main_R >/dev/null && echo "official app running"

S=/vol/stick
cp /work/apps/crashy $S/crashy
cp /work/apps/secret.enc $S/secret.enc
cp /work/apps/my_fw $S/my_fw
printf 'default=hello\ntimeout=3\nssh=0\n' > $S/bootmenu.conf
printf 'greeting=hi there\n' > $S/hello.conf

sh /work/iso/autoexec.sh $S
L=$S/bootmenu.log
wait_for "runner: hello exited" $L
wait_for "starting official" $L 10
sleep 1
check "the stub logged the hook" "autoexec.sh stub: hook ran" $L
check "menu fell back to the default without a screen" "menu: no screen; the default entry was chosen" $L
check "official app stopped before the app" "runner: launching hello" $L
check "embedded app unpacked" "embedded app hello unpacked to RAM" $L
check "app got its conf as HELLO_*" "HELLO_GREETING=hi there" $L
check "app args from app.ini" "arg1=--demo" $L
check "/root/data redirected to the stick" "/root/data writable=yes" $L
[ -f $S/hello-data/hello/written-by-hello ] && echo "PASS data landed on the stick" || { echo "FAIL data not on the stick"; fail=1; }
[ -f /root/data/hello/written-by-hello ] && { echo "FAIL data landed on the unit's /root/data"; fail=1; } || echo "PASS nothing written to the unit's /root/data"
check "after a headless run the official app comes back" "runner: starting official" $L
pidof EP085_Main_R >/dev/null && echo "PASS official app running again" || { echo "FAIL official app not running"; fail=1; }
[ ! -f $S/bootmenu.last ] && echo "PASS a no-screen default does not change the remembered choice" \
  || { echo "FAIL bootmenu.last written without a user choice"; fail=1; }

CTL=/tmp/bootmenu/ctl.sh
sh $CTL list > /tmp/list.txt 2>&1; cat /tmp/list.txt
check "ctl list shows the stick app" "crashy@stick" /tmp/list.txt
sh $CTL run crashy
wait_for "runner: crashy exited" $L 20
check "a failing app is reported" "crashy stopped right away (exit code 3" $L
wait_for "runner: menu exited" $L 10
sh $CTL status
cp /work/apps/hello /tmp/hello.new
sh $CTL update hello2 < /tmp/hello.new
wait_for "runner: hello2 exited" $L 20
[ -f $S/hello2 ] && echo "PASS ctl update wrote the app beside autoexec.bin" || { echo "FAIL no $S/hello2"; fail=1; }
check "updated app ran from RAM" "hello2: copied to RAM" $L

echo "--- an encrypted app"
sh $CTL run secret
wait_for "runner: secret exited" $L 20
check "stick apps get a RAM disk" "RAM disk for apps: /tmp/bootmenu/run" $L
check "it is decrypted into RAM" "secret: decrypted to /tmp/bootmenu/run/secret.bin" $L
check "and runs" "HELLO argc=1 .*BOOTMENU_APP_ID=secret" $L
echo "--- the official application, encrypted, under another name"
sh $CTL run my_fw
wait_for "runner: my_fw exited" $L 20
check "it runs in /root/pdj from RAM" "FAKEFW argv0=/tmp/bootmenu/run/my_fw.bin cwd=/root/pdj" $L
check "it starts with a copy of the unit's settings" "FAKEFW .* dat=unit settings" $L
[ -f $S/my_fw-data/written-by-fw ] && echo "PASS its settings stay on the stick" || { echo "FAIL no $S/my_fw-data/written-by-fw"; fail=1; }
[ -f /root/data/written-by-fw ] && { echo "FAIL it wrote to the unit's /root/data"; fail=1; } || echo "PASS nothing written to the unit's /root/data"

echo "--- stick removed"
mv $S /vol/gone 2>/dev/null; rm -f /tmp/bootmenu/runner.pid.keep
# the watcher keys on the block device; here the stick is a folder, so end the session the way the watcher does
touch /tmp/bootmenu/gone; p=$(cat /tmp/bootmenu/child.pid 2>/dev/null); [ -n "$p" ] && kill $p
sleep 3
grep -q "session over" /vol/gone/bootmenu.log && echo "PASS session ends" || { echo "FAIL session did not end"; fail=1; }
echo "--- log"; cat /vol/gone/bootmenu.log
exit $fail
TEST

# 1) the programs, built in the container (its C library is the one they run against)
docker run --rm --user "$(id -u):$(id -g)" -v "$here":/src:ro -v "$work":/work "$image" sh -c '
  set -e
  mkdir -p /work/iso /work/apps /work/vol
  cc -std=gnu99 -O2 -DBM_DEV_HOST -o /work/iso/bootmenu /src/src/*.c -ldl -lm
  cc -O2 -o /work/apps/hello /work/hello.c
  cc -O2 -o /work/apps/crashy /work/crashy.c
  cc -O2 -Wl,--disable-new-dtags -Wl,-rpath,/usr/X11R6/lib/:/home/iMX6/ltib/rootfs/usr/lib/ -o /work/apps/fakefw /work/fakefw.c'
# 2) encrypted with the throwaway key on the host
python3 "$here/tools/encrypt-app.py" --key "$work/test.key" "$work/apps/hello" --out "$work/apps/secret.enc" >/dev/null
python3 "$here/tools/encrypt-app.py" --key "$work/test.key" "$work/apps/fakefw" --out "$work/apps/my_fw" >/dev/null
# 3) the test, as root in a container standing in for the unit
docker run --rm --cap-add SYS_ADMIN --security-opt apparmor=unconfined --security-opt seccomp=unconfined \
  -v "$here":/src:ro -v "$work":/work "$image" sh -c '
  set -e
  cp /work/apps/hello /work/iso/apps/hello/hello
  printf "name=Hello\nargs=--demo\n" > /work/iso/apps/hello/app.ini
  rm -rf /vol; ln -s /work/vol /vol
  rc=0; /work/bin/busybox sh /work/test.sh || rc=$?
  chown -R '"$(id -u):$(id -g)"' /work; exit $rc'
