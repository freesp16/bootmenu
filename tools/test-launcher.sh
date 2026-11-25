#!/bin/sh
# test-launcher.sh - development only: run image/autoexec.sh end to end in a container standing in for the unit.
#
# BusyBox (a static one from the host, $BUSYBOX) provides the shell and the applets, as on the unit. A fake official app
# (/root/pdj/EP085_Main_R, a shell loop), a fake stick (a folder, so the stick-removal watcher stays quiet until the
# test removes it), the menu built natively with -DBM_DEV_HOST (it lists x86-64 programs), and two test apps:
# `hello` (prints what the launcher gave it, then quits) and `crashy` (fails at once), and with a throwaway key in the
# unit's key path, `secret` (hello encrypted) and `my_fw` (a stand-in for the official application, encrypted, under
# another name). There is no GPU and no X here, so the menu takes its no-screen path: gl-fb fails, gl-x11 fails,
# --fallback picks the default. The stick is a real mount with a device node, pulled at the end while the stand-in
# official application runs; bind mounts are global, as on units without unshare.
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
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static void first_line(const char *path, char *out, size_t n) {
    FILE *f = fopen(path, "r");
    out[0] = 0;
    if (f) { if (!fgets(out, (int)n, f)) out[0] = 0; fclose(f); }
    out[strcspn(out, "\n")] = 0;
}
int main(int argc, char **argv) {
    char cwd[256], dat[64], skin[64], base[64], added[64];
    first_line("/root/data/Toraiz_SP16.dat", dat, sizeof dat);
    first_line("/root/gui/Resource/skin.txt", skin, sizeof skin);
    first_line("/root/gui/Resource/base.txt", base, sizeof base);
    first_line("/root/gui/Resource/added.txt", added, sizeof added);
    printf("FAKEFW argv0=%s cwd=%s dat=%s skin=%s base=%s added=%s\n", argv[0], getcwd(cwd, sizeof cwd), dat, skin,
           base, added);
    FILE *f = fopen("/root/data/written-by-fw", "w");
    if (f) fclose(f);
    fflush(stdout);
    sleep(argc > 1 ? (unsigned)atoi(argv[1]) : 2);
    return 0;
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

# the unit: a official app, its settings in /root/data, its screen resources in /root/gui
mkdir -p /root/pdj /root/data /root/gui/Resource
echo "unit settings" > /root/data/Toraiz_SP16.dat
echo "unit skin" > /root/gui/Resource/skin.txt
echo "unit base" > /root/gui/Resource/base.txt
printf '#!/bin/sh\ntrap "exit 0" TERM\nwhile :; do sleep 1; done\n' > /root/pdj/EP085_Main_R
chmod 0755 /root/pdj/EP085_Main_R
( cd /root/pdj && ./EP085_Main_R & )
sleep 1; pidof EP085_Main_R >/dev/null && echo "official app running"

# the stick: a mount whose device node goes away when it is pulled, as on the unit
S=/stick
mkdir -p $S && mknod /dev/fakestick b 7 200 && mount -t tmpfs /dev/fakestick $S
export BOOTMENU_MOUNT_MODE=global
cp /work/apps/crashy $S/crashy
cp /work/apps/secret.enc $S/secret.enc
cp /work/apps/my_fw $S/my_fw
mkdir -p $S/my_fw-gui/Resource         # only what differs: one changed file, one new one
echo "mod skin" > $S/my_fw-gui/Resource/skin.txt
echo "mod added" > $S/my_fw-gui/Resource/added.txt
printf 'default=hello\ntimeout=3\nssh=0\n' > $S/bootmenu.conf
printf 'greeting=hi there\n' > $S/hello.conf

orig_fw_pid=$(pidof EP085_Main_R)
touch $S/SP16_SAFE_MODE
sh /work/iso/autoexec.sh $S
check "SP16_SAFE_MODE bypasses launcher" "safe mode flag present" $S/bootmenu.log
[ "$(pidof EP085_Main_R)" = "$orig_fw_pid" ] && [ ! -f /tmp/bootmenu/runner.pid ] \
  && echo "PASS safe mode left official app running (same pid)" \
  || { echo "FAIL safe mode modified official app or started runner"; fail=1; }
rm -f $S/SP16_SAFE_MODE $S/bootmenu.log

printf 'default=hello\ntimeout=3\nssh=0\ncold_boot_wait_s=5\n' > $S/bootmenu.conf
cp $B/sh /tmp/apl_start && chmod 0755 /tmp/apl_start
/tmp/apl_start -c 'sleep 2' &
apl_pid=$!
sh /work/iso/autoexec.sh $S
L=$S/bootmenu.log
wait_for "runner: hello exited" $L 30
wait_for "starting official" $L 15
wait $apl_pid 2>/dev/null || true
rm -f /tmp/apl_start
sleep 1
check "cold boot waited for apl_start" "cold boot: waiting for apl_start" $L
check "cold boot waited until apl_start finished" "cold boot: apl_start finished after [1-5]s" $L
check "the stub logged the hook" "autoexec.sh stub: hook ran" $L
check "menu fell back to the default without a screen" "menu: no screen; the default entry was chosen" $L
check "official app stopped before the app" "runner: launching hello" $L
check "embedded app unpacked" "embedded app hello unpacked to RAM" $L
grep -qF " /tmp/bootmenu/run tmpfs " /proc/mounts \
  && echo "PASS /tmp/bootmenu/run is mounted as tmpfs" || { echo "FAIL /tmp/bootmenu/run not tmpfs"; fail=1; }
[ -L /tmp/bootmenu/apps/hello ] && [ -d /tmp/bootmenu/run/embedded/hello ] \
  && echo "PASS embedded app staged in /tmp/bootmenu/run/embedded" \
  || { echo "FAIL embedded app not staged in /tmp/bootmenu/run/embedded"; fail=1; }
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
[ -x /tmp/bootmenu/run/hello2.bin ] && echo "PASS updated app cached in /tmp/bootmenu/run" || { echo "FAIL /tmp/bootmenu/run/hello2.bin missing"; fail=1; }
if ls /tmp/bootmenu/run/update.* >/dev/null 2>&1; then echo "FAIL leftover update.* in /tmp/bootmenu/run"; fail=1
else echo "PASS ctl update staged cleanly in /tmp/bootmenu/run"; fi

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
check "its -gui folder is laid over the unit's screen resources" "FAKEFW .* skin=mod skin base=unit base added=mod added" $L
check "...in RAM" "my_fw: screen resources: the unit's with 2 file(s) of $S/my_fw-gui over them, in RAM" $L
[ "$(cat /root/gui/Resource/skin.txt)" = "unit skin" ] && [ ! -e /root/gui/Resource/added.txt ] \
  && echo "PASS the unit's /root/gui is untouched" || { echo "FAIL the unit's /root/gui changed"; fail=1; }
[ -f $S/my_fw-data/written-by-fw ] && echo "PASS its settings stay on the stick" || { echo "FAIL no $S/my_fw-data/written-by-fw"; fail=1; }
[ -f /root/data/written-by-fw ] && { echo "FAIL it wrote to the unit's /root/data"; fail=1; } || echo "PASS nothing written to the unit's /root/data"

grep -q " /root/data \| /root/gui " /proc/mounts && { echo "FAIL a bind mount was left after the app"; fail=1; } \
  || echo "PASS the bind mounts are undone after each app"

echo "--- the stick is pulled while the official application runs"
echo "args=60" > $S/my_fw.ini
n=$(grep -c "FAKEFW" $L)
sh $CTL run my_fw
i=0; while [ "$(grep -c FAKEFW $L)" -le "$n" ] && [ $i -lt 20 ]; do sleep 1; i=$((i+1)); done
grep -q " /root/gui " /proc/mounts && grep -q " /root/data " /proc/mounts \
  && echo "PASS its resources and settings are bound (globally) while it runs" || { echo "FAIL not bound"; fail=1; }
cp $L /work/vol/before-pull.log
umount -l $S; rm -f /dev/fakestick
E=/tmp/bootmenu/end.log
wait_for "session over" $E 20
check "the watcher sees the stick go" "watcher: .* stick removed (/dev/fakestick)" $E
check "the application is stopped" "watcher: pid [0-9]* ended" $E
pidof my_fw.bin >/dev/null && { echo "FAIL the application still runs"; fail=1; } || echo "PASS the application is gone"
n=$(pidof EP085_Main_R | wc -w)
[ "$n" = 1 ] && echo "PASS the official software runs again, once" || { echo "FAIL official instances: $n"; fail=1; }
grep -q " /root/data \| /root/gui " /proc/mounts && { echo "FAIL a bind mount survived the pull"; grep root /proc/mounts; fail=1; } \
  || echo "PASS the bind mounts are undone"
[ "$(cat /root/data/Toraiz_SP16.dat)" = "unit settings" ] && [ "$(cat /root/gui/Resource/skin.txt)" = "unit skin" ] \
  && echo "PASS the unit's own /root/data and /root/gui are back" || { echo "FAIL the unit's files are not back"; fail=1; }
check "the session ends" "runner: session over" $E

echo "--- the stick goes back in"
mknod /dev/fakestick b 7 201 && mount -t tmpfs /dev/fakestick $S
printf 'ssh=0\n' > $S/bootmenu.conf
sh /work/iso/autoexec.sh $S
wait_for "runner: menu exited" $S/bootmenu.log 20
check "the new log shows how the last session ended" "the end of the previous session, after its stick was removed" $S/bootmenu.log
check "...with its lines" "watcher: pid [0-9]* ended" $S/bootmenu.log
echo "--- log (before the pull)"; cat /work/vol/before-pull.log
echo "--- end of that session"; cat $E 2>/dev/null; sed -n '/the end of the previous session/,/^---$/p' $S/bootmenu.log

stop_runner_clean() {
  touch /tmp/bootmenu/runner.done 2>/dev/null || true
  p=$(cat /tmp/bootmenu/child.pid 2>/dev/null || true); [ -n "$p" ] && kill -KILL "$p" 2>/dev/null || true
  p=$(cat /tmp/bootmenu/runner.pid 2>/dev/null || true); [ -n "$p" ] && kill -KILL "$p" 2>/dev/null || true
  sleep 1
  umount -l /tmp/bootmenu/run 2>/dev/null || true
  rm -rf /tmp/bootmenu
}

echo "--- gl-x11 crash fallback (rc=139 without choice.sh)"
stop_runner_clean
mkdir -p /tmp/iso_crash && cp -R /work/iso/. /tmp/iso_crash/
mv /tmp/iso_crash/bootmenu /tmp/iso_crash/bootmenu.real
cat > /tmp/iso_crash/bootmenu <<'CRASHW'
#!/bin/sh
case " $* " in
  *" --render x11 "*" --fallback "*|*" --fallback "*" --render x11 "*)
    echo "WRAPPER_X11_CRASH" >> /tmp/wrapper.log
    exit 139
    ;;
  *" --resolve "*)
    echo "WRAPPER_RESOLVE $*" >> /tmp/wrapper.log
    ;;
esac
exec /tmp/iso_crash/bootmenu.real "$@"
CRASHW
chmod 0755 /tmp/iso_crash/bootmenu
cp /work/apps/crashy $S/crashy
printf 'default=hello\nssh=0\n' > $S/bootmenu.conf
rm -f $S/bootmenu.log /tmp/wrapper.log
sh /tmp/iso_crash/autoexec.sh $S
L=$S/bootmenu.log
wait_for "runner: hello exited" $L 20
wait_for "starting official" $L 15
sleep 1
check "gl-x11 rc=139 triggered headless default resolve" "gl-x11 failed without a choice (rc=139); resolving default headlessly" $L
check "wrapper simulated x11 crash" "WRAPPER_X11_CRASH" /tmp/wrapper.log
check "wrapper recorded --resolve default" "WRAPPER_RESOLVE .*--resolve default" /tmp/wrapper.log
check "FALLBACK=1 restored official app after app exit" "runner: starting official EP085_Main_R" $L
pidof EP085_Main_R >/dev/null && echo "PASS official app running after crash fallback" || { echo "FAIL official app not running after fallback"; fail=1; }

sh $CTL run crashy
wait_for "resolving official headlessly" $L 20
wait_for "starting official" $L 15
sleep 1
check "non-empty MSG resolves official on gl-x11 crash" "gl-x11 failed without a choice (rc=139); resolving official headlessly" $L
check "wrapper recorded --resolve official" "WRAPPER_RESOLVE .*--resolve official" /tmp/wrapper.log
pidof EP085_Main_R >/dev/null && echo "PASS official app running after MSG fallback" || { echo "FAIL official app not running after MSG fallback"; fail=1; }

echo "--- poweroff standby path (rc=9)"
stop_runner_clean
killall -TERM EP085_Main_R 2>/dev/null || true
sleep 1
killall -KILL EP085_Main_R 2>/dev/null || true
mkdir -p /tmp/iso_pwr && cp -R /work/iso/. /tmp/iso_pwr/
cat > /tmp/iso_pwr/bootmenu <<'PWRSTUB'
#!/bin/sh
case " $* " in
  *" --wake-backlight "*|*" --list "*|*" --resolve "*) exit 0 ;;
esac
exit 9
PWRSTUB
chmod 0755 /tmp/iso_pwr/bootmenu
printf '#!/bin/sh\necho "$0" > /tmp/poweroff.called\n' > $B/poweroff
cp $B/poweroff $B/halt
chmod 0755 $B/poweroff $B/halt
sleep 60 &
fake_db_pid=$!
mkdir -p /tmp/bootmenu
echo "$fake_db_pid" > /tmp/bootmenu/dropbear.pid
printf 'ssh=0\n' > $S/bootmenu.conf
rm -f $S/bootmenu.log /tmp/poweroff.called
sh /tmp/iso_pwr/autoexec.sh $S
L=$S/bootmenu.log
wait_for "runner: session over" $L 15
i=0; while [ ! -f /tmp/poweroff.called ] && [ $i -lt 10 ]; do sleep 1; i=$((i+1)); done
check "rc=9 sets standby requested" "runner: menu: power button requested standby" $L
check "rc=9 logs standby halt without official restart" "standby requested; halting without restarting official software" $L
pidof EP085_Main_R >/dev/null && { echo "FAIL rc=9 restarted official software"; fail=1; } || echo "PASS rc=9 did not restart official software"
! kill -0 "$fake_db_pid" 2>/dev/null && [ ! -f /tmp/bootmenu/dropbear.pid ] \
  && echo "PASS rc=9 stopped dropbear.pid" || { echo "FAIL rc=9 left dropbear.pid running"; fail=1; }
[ -f /tmp/poweroff.called ] && echo "PASS rc=9 invoked poweroff/halt" || { echo "FAIL rc=9 did not invoke poweroff/halt"; fail=1; }
rm -rf /tmp/iso_crash /tmp/iso_pwr /tmp/wrapper.log /tmp/poweroff.called $B/poweroff $B/halt
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
