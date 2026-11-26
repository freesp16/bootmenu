#!/bin/sh
# autoexec.sh - the stub the USB autoexec hook of the SP-16 or the DJS-1000 runs (as root, from the decrypted image, $1 = the stick).
# Deliberately trivial, so even an old shell runs its first line: it logs that the hook ran and how, then runs the
# real launcher (launcher.sh) with every message, parse errors included, in bootmenu.log on the stick.
L="${1:-/tmp}/bootmenu.log"
echo "=== autoexec.sh stub: hook ran $(date 2>/dev/null) args: $*" >> "$L"
D=$(dirname "$0")
case "$D" in /*) ;; *) D="$(pwd)/$D";; esac
echo "stub: dir=$D pwd=$(pwd) uid=$(id -u 2>/dev/null) ppid=$PPID parent=$(tr '\000' ' ' < /proc/$PPID/cmdline 2>/dev/null)" >> "$L"
echo "stub: shell $(readlink /bin/sh 2>/dev/null) $(busybox 2>&1 | head -n 1)" >> "$L"
echo "stub: stick $(grep " $1 " /proc/mounts 2>/dev/null)" >> "$L"
echo "stub: image $(ls -l "$D" 2>&1 | tr '\n' ' ')" >> "$L"
sync
sh "$D/launcher.sh" "$1" "$D" >> "$L" 2>&1
echo "stub: launcher.sh returned $?" >> "$L"
sync
exit 0
