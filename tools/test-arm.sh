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
docker run --rm --user "$(id -u):$(id -g)" -v "$here":/work:ro -v "$t":/t "$image" sh -eu -c '
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
  cc -o /t/stick/native /t/plain.c              # an x86-64 program: must be skipped

  cat > /t/panel_preload.c <<'"'"'PRELOAD'"'"'
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum {
    REQ_MESSAGE_1    = 0x40107000UL,
    REQ_SET_TIMER    = 0x40017001UL,
    REQ_SET_INTERVAL = 0x40047002UL,
    REQ_SET_BITS     = 0x40027003UL,
    REQ_SET_BYTES    = 0x40047004UL,
    REQ_SET_ENDIAN   = 0x40017009UL,
};

struct subucom_ioc_transfer {
    uint8_t  multi_id;
    uint8_t  _reserved;
    uint8_t  part_id;
    uint8_t  part_num;
    uint32_t bytes;
    uint64_t tx_buf;
};

static int (*real_open)(const char *, int, ...);
static int (*real_open64)(const char *, int, ...);
static int (*real_ioctl)(int, unsigned long, ...);
static int (*real_close)(int);

static int fake_fd = -1;
static int step = 0;
static int msg_count = 0;
static int failed = 0;
static struct timespec t_msg2;

static void log_msg(const char *msg) {
    const char *lp = getenv("FAKE_PANEL_LOG");
    if (lp && *lp) {
        FILE *f = fopen(lp, "a");
        if (f) { fputs(msg, f); fclose(f); }
    }
}

static void fail_msg(const char *msg) {
    char buf[256];
    failed = 1;
    snprintf(buf, sizeof buf, "PRELOAD_FAIL step=%d: %s\n", step, msg);
    log_msg(buf);
    fprintf(stderr, "%s", buf);
}

static uint16_t crc_x25(const uint8_t *d, int n) {
    uint16_t crc = 0xffff;
    for (int i = 0; i < n; i++) {
        crc ^= d[i];
        for (int k = 0; k < 8; k++)
            crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0x8408) : (uint16_t)(crc >> 1);
    }
    return (uint16_t)~crc;
}

static void init_syms(void) {
    if (!real_open)   real_open   = dlsym(RTLD_NEXT, "open");
    if (!real_open64) real_open64 = dlsym(RTLD_NEXT, "open64");
    if (!real_ioctl)  real_ioctl  = dlsym(RTLD_NEXT, "ioctl");
    if (!real_close)  real_close  = dlsym(RTLD_NEXT, "close");
}

static int handle_open(const char *path, int flags, mode_t mode, int use64) {
    init_syms();
    const char *target = getenv("FAKE_PANEL_PATH");
    if (target && path && strcmp(path, target) == 0) {
        int fd = real_open("/dev/null", O_RDONLY);
        if (fd >= 0) {
            fake_fd = fd;
            step = 0;
            msg_count = 0;
            failed = 0;
        }
        return fd;
    }
    int (*fn)(const char *, int, ...) = use64 && real_open64 ? real_open64 : real_open;
    if (flags & O_CREAT) return fn(path, flags, mode);
    return fn(path, flags);
}

int open(const char *path, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }
    return handle_open(path, flags, mode, 0);
}

int open64(const char *path, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }
    return handle_open(path, flags, mode, 1);
}

int ioctl(int fd, unsigned long req, ...) {
    init_syms();
    va_list ap;
    va_start(ap, req);
    void *arg = va_arg(ap, void *);
    va_end(ap);
    if (fake_fd < 0 || fd != fake_fd) return real_ioctl(fd, req, arg);

    switch (step) {
    case 0:
        if (req != REQ_SET_TIMER || !arg || *(const uint8_t *)arg != 0) {
            fail_msg("expected REQ_SET_TIMER=0 first (Trap A)"); errno = EINVAL; return -1;
        }
        step = 1; return 0;
    case 1:
        if (req != REQ_SET_BITS || !arg || *(const uint16_t *)arg != 32) {
            fail_msg("expected REQ_SET_BITS=32"); errno = EINVAL; return -1;
        }
        step = 2; return 0;
    case 2:
        if (req != REQ_SET_ENDIAN || !arg || *(const uint8_t *)arg != 1) {
            fail_msg("expected REQ_SET_ENDIAN=1"); errno = EINVAL; return -1;
        }
        step = 3; return 0;
    case 3:
        if (req != REQ_SET_BYTES || !arg || *(const uint32_t *)arg != 128) {
            fail_msg("expected REQ_SET_BYTES=128"); errno = EINVAL; return -1;
        }
        step = 4; return 0;
    case 4:
        if (req != REQ_SET_INTERVAL || !arg || *(const uint32_t *)arg != 3) {
            fail_msg("expected REQ_SET_INTERVAL=3"); errno = EINVAL; return -1;
        }
        step = 5; return 0;
    case 5:
        if (req != REQ_SET_TIMER || !arg || *(const uint8_t *)arg != 1) {
            fail_msg("expected REQ_SET_TIMER=1 before REQ_MESSAGE_1 (Trap B)"); errno = EINVAL; return -1;
        }
        step = 6; return 0;
    case 6:
    case 7: {
        if (req != REQ_MESSAGE_1 || !arg) {
            fail_msg("expected REQ_MESSAGE_1"); errno = EINVAL; return -1;
        }
        const struct subucom_ioc_transfer *x = (const struct subucom_ioc_transfer *)arg;
        const uint8_t *tx = (const uint8_t *)(uintptr_t)x->tx_buf;
        if (x->bytes != 128 || !tx) {
            fail_msg("invalid TX transfer size/buffer"); errno = EINVAL; return -1;
        }
        uint8_t want_standby = (step == 6) ? 0 : 1;
        uint16_t got_crc = (uint16_t)(tx[124] | ((uint16_t)tx[125] << 8));
        uint16_t want_crc = crc_x25(tx, 124);
        if (tx[0] != 0x02 || tx[102] != 1 || tx[103] != want_standby || tx[119] != 0xAA || got_crc != want_crc) {
            char m[160];
            snprintf(m, sizeof m, "bad TX frame #%d (hdr=0x%02x hm=%u sb=%u magic=0x%02x crc=0x%04x/0x%04x)",
                     step - 5, tx[0], tx[102], tx[103], tx[119], got_crc, want_crc);
            fail_msg(m); errno = EINVAL; return -1;
        }
        msg_count++;
        if (step == 7) clock_gettime(CLOCK_MONOTONIC, &t_msg2);
        step++;
        return 0;
    }
    case 8: {
        if (req != REQ_SET_TIMER || !arg || *(const uint8_t *)arg != 0) {
            fail_msg("expected final REQ_SET_TIMER=0"); errno = EINVAL; return -1;
        }
        struct timespec t_off;
        clock_gettime(CLOCK_MONOTONIC, &t_off);
        double ms = (double)(t_off.tv_sec - t_msg2.tv_sec) * 1000.0 +
                    (double)(t_off.tv_nsec - t_msg2.tv_nsec) / 1e6;
        if (ms < 8.0) {
            char m[128];
            snprintf(m, sizeof m, "Trap C violated: elapsed %.2f ms < 8.0 ms", ms);
            fail_msg(m); errno = EINVAL; return -1;
        }
        char ok[128];
        snprintf(ok, sizeof ok, "PRELOAD_PASS tx_frames=%d elapsed_ms=%.2f\n", msg_count, ms);
        log_msg(ok);
        step = 9;
        return 0;
    }
    default:
        fail_msg("unexpected extra ioctl"); errno = EINVAL; return -1;
    }
}

int close(int fd) {
    init_syms();
    if (fake_fd >= 0 && fd == fake_fd) {
        if (step != 9 || msg_count != 2) fail_msg("close() before standby sequence completed");
        fake_fd = -1;
    }
    return real_close(fd);
}
PRELOAD
  arm-linux-gnueabi-gcc -shared -fPIC -O2 -Wall -Werror --sysroot=/opt/sp16/sysroot -o /t/panel_preload.so /t/panel_preload.c -ldl

  cat > /t/panel_unit.c <<'"'"'UNIT'"'"'
#include "bm.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

void bm_log(const char *fmt, ...) { (void)fmt; }

static uint16_t crc_x25(const uint8_t *d, int n) {
    uint16_t crc = 0xffff;
    for (int i = 0; i < n; i++) {
        crc ^= d[i];
        for (int k = 0; k < 8; k++)
            crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0x8408) : (uint16_t)(crc >> 1);
    }
    return (uint16_t)~crc;
}

static void make_rx(uint8_t *f, int pwr_bit, int bit62, int rx, int ry) {
    memset(f, 0, 128);
    f[0] = 0x01;
    if (pwr_bit) f[5] |= (1u << 4);
    if (bit62) f[62] |= 1u;
    f[0x2c] = (uint8_t)((rx >> 8) & 0xff);
    f[0x2d] = (uint8_t)(rx & 0xff);
    f[0x2e] = (uint8_t)((ry >> 8) & 0xff);
    f[0x2f] = (uint8_t)(ry & 0xff);
    uint16_t crc = crc_x25(f, 124);
    f[124] = (uint8_t)(crc & 0xff);
    f[125] = (uint8_t)((crc >> 8) & 0xff);
}

static int count_kind(const InputEvent *ev, int n, InputKind k) {
    int c = 0;
    for (int i = 0; i < n; i++) if (ev[i].kind == k) c++;
    return c;
}

int main(void) {
    uint8_t f[128];
    InputEvent ev[16];
    int n, fail = 0;

    /* 1) Initial frames with RX[5] bit 4 == 0 (before released 1 seen) do NOT emit IN_POWER. */
    panel_reset();
    int pwr = 0;
    for (int i = 0; i < 30; i++) {
        make_rx(f, 0, 0, 0, 0);
        n = panel_decode_frame(f, ev, 16);
        if (n < 0) { printf("FAIL init decode\n"); return 1; }
        pwr += count_kind(ev, n, IN_POWER);
    }
    if (pwr != 0) { printf("FAIL 1: initial pressed emitted %d IN_POWER\n", pwr); fail = 1; }
    else printf("PASS 1: initial RX[5] bit4==0 suppressed\n");

    /* 2) Toggling RX[62] & 1 does NOT emit IN_POWER. */
    panel_reset();
    pwr = 0;
    for (int i = 0; i < 20; i++) {
        make_rx(f, 1, 0, 0, 0);
        n = panel_decode_frame(f, ev, 16);
        pwr += count_kind(ev, n, IN_POWER);
    }
    for (int i = 0; i < 40; i++) {
        make_rx(f, 1, i & 1, 0, 0);
        n = panel_decode_frame(f, ev, 16);
        pwr += count_kind(ev, n, IN_POWER);
    }
    if (pwr != 0) { printf("FAIL 2: RX[62]&1 emitted %d IN_POWER\n", pwr); fail = 1; }
    else printf("PASS 2: RX[62]&1 toggle ignored\n");

    /* 3) 17 released frames arm; 16 pressed do NOT emit; 17th pressed emits once; extra pressed do not re-fire. */
    panel_reset();
    for (int i = 0; i < 17; i++) {
        make_rx(f, 1, 0, 0, 0);
        n = panel_decode_frame(f, ev, 16);
        if (count_kind(ev, n, IN_POWER) != 0) { printf("FAIL 3a: IN_POWER during arm\n"); fail = 1; }
    }
    for (int i = 0; i < 16; i++) {
        make_rx(f, 0, 0, 0, 0);
        n = panel_decode_frame(f, ev, 16);
        if (count_kind(ev, n, IN_POWER) != 0) { printf("FAIL 3b: IN_POWER at pressed frame %d\n", i + 1); fail = 1; }
    }
    make_rx(f, 0, 0, 0, 0);
    n = panel_decode_frame(f, ev, 16);
    if (count_kind(ev, n, IN_POWER) != 1) { printf("FAIL 3c: expected 1 IN_POWER on 17th pressed\n"); fail = 1; }
    for (int i = 0; i < 20; i++) {
        make_rx(f, 0, 0, 0, 0);
        n = panel_decode_frame(f, ev, 16);
        if (count_kind(ev, n, IN_POWER) != 0) { printf("FAIL 3d: re-fired while held\n"); fail = 1; }
    }
    for (int i = 0; i < 17; i++) { make_rx(f, 1, 0, 0, 0); panel_decode_frame(f, ev, 16); }
    for (int i = 0; i < 16; i++) {
        make_rx(f, 0, 0, 0, 0);
        n = panel_decode_frame(f, ev, 16);
        if (count_kind(ev, n, IN_POWER) != 0) { printf("FAIL 3e: early re-arm\n"); fail = 1; }
    }
    make_rx(f, 0, 0, 0, 0);
    n = panel_decode_frame(f, ev, 16);
    if (count_kind(ev, n, IN_POWER) != 1) { printf("FAIL 3f: second press failed\n"); fail = 1; }
    if (!fail) printf("PASS 3: 17-frame release arm + 17-frame press debounce\n");

    /* 4) panel_set_touch_cal() valid vs invalid span (< 100), preserving defaults (940,72,87,866). */
    int tx = -1, ty = -1;
    panel_set_touch_cal(100, 120, 50, 60);
    panel_set_touch_cal(100, 150, 87, 866);
    panel_set_touch_cal(940, 72, 100, 150);
    panel_reset();
    for (int i = 0; i < 3; i++) {
        make_rx(f, 1, 0, 940, 87);
        n = panel_decode_frame(f, ev, 16);
        for (int j = 0; j < n; j++) if (ev[j].kind == IN_TOUCH_DOWN) { tx = ev[j].x; ty = ev[j].y; }
    }
    if (tx != 40 || ty != 29) { printf("FAIL 4a: invalid cal changed default (%d,%d)\n", tx, ty); fail = 1; }
    panel_set_touch_cal(946, 75, 101, 882);
    panel_reset();
    tx = ty = -1;
    for (int i = 0; i < 3; i++) {
        make_rx(f, 1, 0, 946, 101);
        n = panel_decode_frame(f, ev, 16);
        for (int j = 0; j < n; j++) if (ev[j].kind == IN_TOUCH_DOWN) { tx = ev[j].x; ty = ev[j].y; }
    }
    if (tx != 40 || ty != 29) { printf("FAIL 4b: custom cal origin (%d,%d)\n", tx, ty); fail = 1; }
    panel_reset();
    tx = ty = -1;
    for (int i = 0; i < 3; i++) {
        make_rx(f, 1, 0, 940, 87);
        n = panel_decode_frame(f, ev, 16);
        for (int j = 0; j < n; j++) if (ev[j].kind == IN_TOUCH_DOWN) { tx = ev[j].x; ty = ev[j].y; }
    }
    if (tx != 45 || ty != 22) { printf("FAIL 4c: custom cal offset (%d,%d) != (45,22)\n", tx, ty); fail = 1; }
    else printf("PASS 4: panel_set_touch_cal() applies valid and rejects span < 100\n");

    return fail;
}
UNIT
  $cc -I/work/src -o /t/panel-unit /t/panel_unit.c /work/src/panel.c'

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
  rc=0; run --wake-backlight --settings $t/settings.dat 2> $t/bl || rc=$?
  check "the backlight follows the official LCD BRIGHTNESS" "LCD BRIGHTNESS 5 -> level 248" $t/bl
  [ $rc -eq 8 ] && echo "PASS --wake-backlight without /dev/backlight fails" || { echo "FAIL --wake-backlight: rc=$rc"; fail=1; }
  run --wake-backlight --settings $t/settings-bad.dat 2> $t/bl2 || true
  check "an out-of-range LCD BRIGHTNESS means the default" "LCD BRIGHTNESS 3 -> level 91" $t/bl2
  run --wake-backlight --settings $t/missing.dat 2> $t/bl3 || true
  check "no settings file means the default" "LCD BRIGHTNESS 3 -> level 91" $t/bl3
  run --version > $t/version
  check "--version prints the build version" "^bootmenu [0-9]" $t/version
  rc=0; run --standby-ack --panel $t/missing-panel 2> $t/standby_miss || rc=$?
  [ $rc -eq 10 ] && echo "PASS --standby-ack missing panel returns rc=10" || { echo "FAIL --standby-ack missing: rc=$rc"; fail=1; }
  check "--standby-ack missing panel logs error" "cannot open $t/missing-panel" $t/standby_miss
  rc=0
  QEMU_CPU=cortex-a9 qemu-arm -E LD_PRELOAD=$t/panel_preload.so -E FAKE_PANEL_PATH=$t/fake-panel -E FAKE_PANEL_LOG=$t/preload.log \
    -L /opt/sp16/sysroot /work/out/bootmenu --standby-ack --panel $t/fake-panel 2> $t/standby_ok || rc=$?
  [ $rc -eq 0 ] && echo "PASS --standby-ack with LD_PRELOAD fake-panel returns rc=0" || { echo "FAIL --standby-ack fake-panel: rc=$rc"; cat $t/standby_ok; fail=1; }
  check "--standby-ack logs completion" "standby ACK sent to $t/fake-panel" $t/standby_ok
  check "LD_PRELOAD verified Trap A+B+C and TX CRC" "^PRELOAD_PASS " $t/preload.log
  if QEMU_CPU=cortex-a9 qemu-arm -L /opt/sp16/sysroot $t/panel-unit; then echo "PASS panel-unit"; else echo "FAIL panel-unit"; fail=1; fi
  echo "touch_cal=100,120,50,60" > $t/stick/bootmenu.conf
  run --stick $t/stick --key $t/test.key --list > /dev/null 2> $t/cal_bad
  check "invalid touch_cal logs warning" "ignoring invalid touch_cal" $t/cal_bad
  echo "touch_cal=946,75,101,882" > $t/stick/bootmenu.conf
  run --stick $t/stick --key $t/test.key --list > /dev/null 2> $t/cal_ok
  if grep -q "ignoring invalid touch_cal" $t/cal_ok; then echo "FAIL valid touch_cal warned"; cat $t/cal_ok; fail=1
  else echo "PASS valid touch_cal accepted without warning"; fi
  exit $fail'
