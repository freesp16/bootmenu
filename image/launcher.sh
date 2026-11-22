#!/bin/sh
#
# launcher.sh - Boot menu, run by autoexec.sh (the stub the SP-16's own USB autoexec hook starts AS ROOT). Shows the boot menu (bootmenu, an
# OpenGL ES program) in place of the official app, then runs what was chosen: an app beside autoexec.bin on the stick, an
# app embedded in this image, or the official software. When an app quits, the menu comes back. Optional SSH server on the
# LINK port (development images only). Nothing is written to internal flash: everything runs from RAM (/tmp/bootmenu) and
# the apps keep their data on the stick.
#
# Call chain on the device (official firmware, nothing modified):
#   udev mounts the FAT stick -> /root/pdj/decrypt_autoexec.sh <stick>
#     -> AES-decrypts autoexec.bin, loop-mounts it, runs ./autoexec.sh <stick>, then unmounts it
#
# $1 = the USB stick mount path (writable FAT32).
#
# The image holds:
#   bootmenu          the menu
#   apps/<id>/        embedded apps (app.ini + the executable), optional
#   dropbearmulti     the Dropbear SSH server, optional (no SSH without it)
#   authorized_keys   public keys allowed to log in as root, optional
#
# On the stick (all optional):
#   <file>, apps/...  apps to list (ARM armel executables, plain or encrypted with the unit's key; see the README)
#   bootmenu.conf     menu keys (default=, timeout=, scan_root=, show_official=, official_wait_ms=) and network keys:
#                       ssh=1  ssh_port=22  ssh_ip=169.254.16.16 (none = no alias)  ssh_netmask=255.255.0.0  ssh_dhcp=0
#                     (without bootmenu.conf, ssh* keys are also read from freesp16.conf, for older sticks)
#   <id>.conf         an app's key=value settings, exported as <ID>_<KEY> (e.g. myapp.conf -> MYAPP_*)
#   bootmenu-ssh/authorized_keys   more public keys (an older freesp16-ssh/ folder is used if it exists)
# Written to the stick:
#   bootmenu.log      this script, the runner, the menu, the SSH server and the apps' output
#   bootmenu.last     the last choice (the menu's default)
#   <id>-data/        an app's /root/data (the stick, never NAND); for the official application run from the stick, a
#                     copy of the unit's own /root/data made the first time
#
# Remote control (ssh root@169.254.16.16, then `ctl help`):
#   ctl status | menu | run ID | start | official | stop | update [ID] < binary | list | log [N] | follow
#
# Quit: pull the stick (the app or menu is stopped and the official app comes back), or power-cycle.

USB="$1"
SELF_DIR="$2"
STAGE=/tmp/bootmenu
LOG="${USB:-/tmp}/bootmenu.log"

{
  echo "==================================================================="
  echo "autoexec.sh (Boot menu) starting   $(date 2>/dev/null)"
  echo "USB=$USB  SELF_DIR=$SELF_DIR  uid=$(id -u 2>/dev/null)"
  if [ -z "$USB" ] || [ ! -d "$USB" ]; then
    echo "ERROR: no USB stick path given"; exit 1
  fi

  # A runner from an earlier insertion that is still alive owns the session: leave it alone.
  old=$(cat "$STAGE/runner.pid" 2>/dev/null)
  if [ -n "$old" ] && kill -0 "$old" 2>/dev/null; then
    echo "autoexec.sh: a runner is already active (pid $old); not starting another"; exit 0
  fi

  rm -rf "$STAGE/apps" 2>/dev/null
  mkdir -p "$STAGE/bin" "$STAGE/apps" "$STAGE/run" 2>/dev/null
  rm -f "$STAGE/gone" "$STAGE/request" "$STAGE/child.pid" "$STAGE/runner.done" "$STAGE/choice.sh" 2>/dev/null

  # 1) The small files first: the menu and the SSH server go to RAM before anything else.
  if ! cp "$SELF_DIR/bootmenu" "$STAGE/bin/bootmenu" 2>/dev/null; then
    echo "ERROR: could not copy the menu out of the image (no room in /tmp?)"; exit 1
  fi
  chmod 0755 "$STAGE/bin/bootmenu"
  if [ -f "$SELF_DIR/dropbearmulti" ] && cp "$SELF_DIR/dropbearmulti" "$STAGE/bin/dropbearmulti" 2>/dev/null; then
    chmod 0755 "$STAGE/bin/dropbearmulti"
  else
    rm -f "$STAGE/bin/dropbearmulti" 2>/dev/null
  fi
  if [ -f "$SELF_DIR/authorized_keys" ]; then cp "$SELF_DIR/authorized_keys" "$STAGE/authorized_keys.image" 2>/dev/null
  else rm -f "$STAGE/authorized_keys.image" 2>/dev/null; fi

  # Embedded apps: a placeholder each (app.ini + .pending), so the menu lists them while they are copied (step 4).
  for d in "$SELF_DIR"/apps/*/; do
    [ -d "$d" ] || continue
    id=$(basename "$d")
    mkdir -p "$STAGE/apps/$id"
    [ -f "$d/app.ini" ] && cp "$d/app.ini" "$STAGE/apps/$id/app.ini"
    touch "$STAGE/apps/$id/.pending"
  done
  echo "$LOG" > "$STAGE/log.path"
  echo "$USB" > "$STAGE/usb.path"

  # 2) The detached runner: the menu at once, network and SSH beside it, then whatever was chosen.
  cat > "$STAGE/run.sh" <<'RUNNER'
#!/bin/sh
LOG_RUN="$1"
USB_RUN="$2"
STAGE=/tmp/bootmenu
exec >>"$LOG_RUN" 2>&1
echo $$ > "$STAGE/runner.pid"
echo "runner: detached, pid $$"

# Flush the log to the stick every few seconds, so it survives pulling the stick or power-cycling.
( while kill -0 $$ 2>/dev/null; do sleep 5; sync; done ) &

# --- Official app --------------------------------------------------------------------------------------------------------
official_pids() {
  for p in $(pidof EP085_Main_R 2>/dev/null); do
    s=$(cut -d' ' -f3 "/proc/$p/stat" 2>/dev/null)
    [ -n "$s" ] && [ "$s" != Z ] && echo "$p"
  done
}
official_running() { [ -n "$(official_pids)" ]; }

# Detached (the subshell exits at once): init reaps the official app, and it outlives the runner.
restore_official() {
  if official_running; then return 0; fi
  if [ -x /root/pdj/EP085_Main_R ]; then
    echo "runner: starting official EP085_Main_R"
    if [ -f "$STAGE/gone" ]; then
      ( cd /root/pdj && ./EP085_Main_R </dev/null >/dev/null 2>&1 & ) # the log was on the stick
    else
      ( cd /root/pdj && ./EP085_Main_R </dev/null & )
    fi
  fi
  sync
}

# The menu stops the official app itself (fast); this is for `ctl run` and for apps started without the menu.
stop_official() {
  official_running || return 0
  echo "runner: stopping official EP085_Main_R"
  killall -TERM EP085_Main_R 2>/dev/null || kill -TERM $(official_pids) 2>/dev/null
  i=0
  while [ $i -lt 10 ]; do
    official_running || break
    sleep 1; i=$((i+1))
  done
  killall -KILL EP085_Main_R 2>/dev/null || kill -KILL $(official_pids) 2>/dev/null
  sleep 1
}

# Diagnostics: every process holding the panel (/dev/subucom_spi0.1) or the strip (/dev/cdc_spi2.0).
log_holders() {
  found=""
  for p in /proc/[0-9]*; do
    for f in "$p"/fd/*; do
      t=$(readlink "$f" 2>/dev/null) || continue
      case "$t" in
        *subucom_spi*|*cdc_spi*)
          echo "runner: holders ($1): pid ${p#/proc/} [$(cat "$p/comm" 2>/dev/null)] fd ${f##*/} -> $t"
          found=1;;
      esac
    done
  done
  [ -n "$found" ] || echo "runner: holders ($1): none"
}

# --- Settings ---------------------------------------------------------------------------------------------------------
# read_conf FILE PREFIX [KEY_PATTERN]: export PREFIX<KEY>=value for each key=value line (keys a-z0-9_).
read_conf() {
  [ -f "$1" ] || return 1
  while IFS= read -r line || [ -n "$line" ]; do
    line=$(printf '%s' "$line" | tr -d '\r')
    case "$line" in ''|'#'*) continue;; esac
    key=${line%%=*}; val=${line#*=}
    case "$key" in *[!a-z0-9_]*|'') continue;; esac
    if [ -n "$3" ]; then case "$key" in $3) ;; *) continue;; esac; fi
    var=$2$(printf '%s' "$key" | tr 'a-z' 'A-Z')
    export "$var=$val"
  done < "$1"
  return 0
}
if [ -f "$USB_RUN/bootmenu.conf" ]; then
  read_conf "$USB_RUN/bootmenu.conf" BOOTMENU_ 'ssh*'
  echo "runner: network settings from bootmenu.conf"
elif read_conf "$USB_RUN/freesp16.conf" BOOTMENU_ 'ssh*'; then
  echo "runner: network settings from freesp16.conf (no bootmenu.conf)"
fi

# --- Network: eth0 (the rear LINK port) up, plus a fixed alias address to reach the unit at. --------------------------
NET_IF=eth0
SSH_IP=${BOOTMENU_SSH_IP:-169.254.16.16}
SSH_MASK=${BOOTMENU_SSH_NETMASK:-255.255.0.0}

alias_present() { ifconfig "$NET_IF:16" 2>/dev/null | grep -qF "addr:$SSH_IP "; }
add_alias() {
  [ "$SSH_IP" = none ] && return 0
  ifconfig "$NET_IF:16" "$SSH_IP" netmask "$SSH_MASK" up 2>/dev/null && return 0
  ip addr add "$SSH_IP/$SSH_MASK" dev "$NET_IF" label "$NET_IF:16" 2>/dev/null
}

setup_network() {
  [ "${BOOTMENU_SSH:-1}" = 0 ] && { echo "runner: net: off (ssh=0)"; return 0; }
  [ -e "/sys/class/net/$NET_IF" ] || { echo "runner: net: no $NET_IF on this unit"; return 1; }
  ifconfig "$NET_IF" up 2>/dev/null || ip link set "$NET_IF" up 2>/dev/null
  if [ "$SSH_IP" = none ]; then echo "runner: net: no alias address (ssh_ip=none)"
  elif add_alias; then echo "runner: net: $NET_IF:16 = $SSH_IP netmask $SSH_MASK"
  else echo "runner: net: WARNING could not add $SSH_IP to $NET_IF"; fi
  if [ "${BOOTMENU_SSH_DHCP:-0}" = 1 ]; then
    if command -v udhcpc >/dev/null 2>&1; then
      cat > "$STAGE/udhcpc.sh" <<'DHCP'
#!/bin/sh
case "$1" in
  bound|renew) ifconfig "$interface:17" "$ip" netmask "${subnet:-255.255.255.0}" up
               echo "udhcpc: $interface:17 = $ip (lease)";;
  deconfig)    ifconfig "$interface:17" down 2>/dev/null;;
esac
exit 0
DHCP
      chmod 0755 "$STAGE/udhcpc.sh"
      udhcpc -i "$NET_IF" -s "$STAGE/udhcpc.sh" -p "$STAGE/udhcpc.pid" -b -t 5 -T 3 -A 10 \
        && echo "runner: net: DHCP client started (the address shows as $NET_IF:17)"
    else
      echo "runner: net: ssh_dhcp=1 but this unit has no udhcpc"
    fi
  fi
  echo "runner: net: carrier=$(cat "/sys/class/net/$NET_IF/carrier" 2>/dev/null || echo '?') (1 = cable in and linked)"
}

# --- SSH: Dropbear from RAM, keys only, host key kept on the stick. ---------------------------------------------------
start_ssh() {
  [ "${BOOTMENU_SSH:-1}" = 0 ] && return 0
  [ -x "$STAGE/bin/dropbearmulti" ] || { echo "runner: ssh: no dropbearmulti in the image, no SSH"; return 1; }
  for n in dropbear dropbearkey scp dbclient; do ln -sf dropbearmulti "$STAGE/bin/$n"; done
  ln -sf "$STAGE/ctl.sh" "$STAGE/bin/ctl"
  # Some Dropbear builds have their session PATH compiled in as /tmp/freesp16/bin:...; point that at ours so `ctl`
  # and `scp` work over ssh (only when nothing else owns that path).
  if [ ! -e /tmp/freesp16/bin ]; then mkdir -p /tmp/freesp16 && ln -s "$STAGE/bin" /tmp/freesp16/bin; fi
  SSHDIR="$USB_RUN/bootmenu-ssh"
  [ -d "$USB_RUN/freesp16-ssh" ] && SSHDIR="$USB_RUN/freesp16-ssh" # keep the earlier launcher's host key
  KEYS="$STAGE/ssh"
  mkdir -p "$KEYS" && chmod 0700 "$KEYS" && chmod 0755 "$STAGE"
  cat "$STAGE/authorized_keys.image" "$SSHDIR/authorized_keys" 2>/dev/null | tr -d '\r' \
    | grep -E '^[^#[:space:]]' > "$KEYS/authorized_keys"
  chmod 0600 "$KEYS/authorized_keys"
  n=$(grep -c . "$KEYS/authorized_keys" 2>/dev/null)
  [ "${n:-0}" -gt 0 ] || { echo "runner: ssh: no public key (image or $SSHDIR/authorized_keys); not starting"; return 1; }
  echo "runner: ssh: $n authorized key(s)"
  HK="$SSHDIR/dropbear_ed25519_host_key"
  mkdir -p "$SSHDIR" 2>/dev/null
  if [ ! -s "$HK" ]; then
    rm -f "$HK" 2>/dev/null
    "$STAGE/bin/dropbearkey" -t ed25519 -f "$HK" >/dev/null 2>&1 || HK=""
  fi
  if [ -z "$HK" ] || ! cp "$HK" "$KEYS/host_key" 2>/dev/null; then
    echo "runner: ssh: WARNING no host key on the stick; using a RAM one"
    rm -f "$KEYS/host_key"
    "$STAGE/bin/dropbearkey" -t ed25519 -f "$KEYS/host_key" >/dev/null 2>&1 \
      || { echo "runner: ssh: ERROR could not make a host key"; return 1; }
  fi
  chmod 0600 "$KEYS/host_key"
  "$STAGE/bin/dropbearkey" -y -f "$KEYS/host_key" 2>/dev/null | sed -n 's/^Fingerprint: /runner: ssh: host key fingerprint /p'
  if ! grep -q ' /dev/pts ' /proc/mounts 2>/dev/null; then
    mkdir -p /dev/pts 2>/dev/null
    mount -t devpts devpts /dev/pts 2>/dev/null && echo "runner: ssh: mounted /dev/pts" \
      || echo "runner: ssh: WARNING no /dev/pts: commands work, interactive terminals do not (use ssh -T)"
  fi
  port=${BOOTMENU_SSH_PORT:-22}
  PATH="$STAGE/bin:$PATH" "$STAGE/bin/dropbear" -F -E -s -r "$KEYS/host_key" -D "$KEYS" -p "$port" -K 15 </dev/null &
  ssh_pid=$!
  sleep 1
  if kill -0 "$ssh_pid" 2>/dev/null; then
    echo "$ssh_pid" > "$STAGE/dropbear.pid"
    echo "runner: ssh: listening on port $port (pid $ssh_pid): ssh root@$SSH_IP"
  else
    echo "runner: ssh: ERROR dropbear exited at start"
  fi
}

stop_ssh() {
  killall -TERM dropbear 2>/dev/null
  p=$(cat "$STAGE/udhcpc.pid" 2>/dev/null); [ -n "$p" ] && kill -TERM "$p" 2>/dev/null
  rm -f "$STAGE/dropbear.pid" "$STAGE/udhcpc.pid"
}

# --- Apps' /root/data off NAND -----------------------------------------------------------------------------------------
# Preferred: a private mount namespace per launch, so only the app sees the swap. Otherwise a global bind mount
# while the app runs, undone before the official app comes back.
if command -v unshare >/dev/null 2>&1 && unshare -m sh -c 'mount -o bind /tmp /root/data' 2>/dev/null; then
  MOUNT_MODE=ns
else
  MOUNT_MODE=global
fi
echo "runner: /root/data redirect: $MOUNT_MODE"
GLOBAL_MOUNT=""

after_app() {
  if [ -n "$GLOBAL_MOUNT" ]; then
    umount /root/data 2>/dev/null || umount -l /root/data 2>/dev/null
    GLOBAL_MOUNT=""
    echo "runner: /root/data restored"
  fi
}

# Stick apps run from a RAM disk of their own: the unit's /tmp is small (often under 10 MB free, less than the official
# application's 12 MB) but its RAM is not, and a program must not depend on the stick while it runs (the official
# application handles USB storage itself). Without the RAM disk, /tmp is used as before.
RUN_RAM_SIZE=128m
run_ramdisk() {
  grep -q " $STAGE/run tmpfs " /proc/mounts 2>/dev/null && return 0
  mkdir -p "$STAGE/run"
  if mount -t tmpfs -o size=$RUN_RAM_SIZE,mode=0755 tmpfs "$STAGE/run" 2>/dev/null; then
    echo "runner: RAM disk for apps: $STAGE/run ($RUN_RAM_SIZE)"
  else
    echo "runner: WARNING no RAM disk for apps; they are copied to /tmp when it has room"
  fi
}

# The executable to run: stick apps are copied to RAM (cached by size + time, one app at a time) unless run_from=stick
# or there is no room; encrypted ones are decrypted there (bootmenu --decrypt, with the unit's key), else on the stick.
app_binary() {
  APP_EXE="$BM_EXE"
  [ "$BM_SOURCE" = stick ] || return 0
  [ "$BM_RUN_FROM" = stick ] && [ "$BM_ENCRYPTED" != 1 ] && return 0
  run_ramdisk
  cache="$STAGE/run/$BM_ID.bin"
  stamp="$BM_SIZE $BM_MTIME"
  if [ -x "$cache" ] && [ "$(cat "$cache.stamp" 2>/dev/null)" = "$stamp" ]; then
    APP_EXE="$cache"; echo "runner: $BM_ID: RAM copy is current"; return 0
  fi
  for f in "$STAGE"/run/*.bin; do [ -e "$f" ] && rm -f "$f" "$f.stamp"; done
  rm -f "$cache.tmp"
  if [ "$BM_ENCRYPTED" = 1 ]; then
    for cache in "$cache" "$USB_RUN/bootmenu-cache/$BM_ID.bin"; do
      mkdir -p "$(dirname "$cache")" 2>/dev/null
      if "$STAGE/bin/bootmenu" --decrypt "$BM_EXE" > "$cache.tmp" && mv "$cache.tmp" "$cache"; then
        chmod 0755 "$cache"; echo "$stamp" > "$cache.stamp"
        APP_EXE="$cache"; echo "runner: $BM_ID: decrypted to $cache"; return 0
      fi
      rm -f "$cache.tmp"
      echo "runner: $BM_ID: could not decrypt to $cache"
    done
    APP_EXE=""; return 1
  fi
  if cp "$BM_EXE" "$cache.tmp" 2>/dev/null && mv "$cache.tmp" "$cache"; then
    chmod 0755 "$cache"; echo "$stamp" > "$cache.stamp"
    APP_EXE="$cache"; echo "runner: $BM_ID: copied to RAM ($BM_SIZE bytes)"
  else
    rm -f "$cache.tmp"
    echo "runner: $BM_ID: no room in RAM, running it from the stick"
  fi
}

# The official application from the stick starts with a copy of the unit's settings (calibration, last state), kept in
# its own folder on the stick, so the installed software's settings never change.
seed_official_data() {
  [ -f "$1/.bootmenu-seeded" ] && return 0
  if cp -R /root/data/. "$1/" 2>/dev/null; then echo "runner: $BM_ID: settings copied from the unit's /root/data"
  else echo "runner: $BM_ID: WARNING not all of /root/data could be copied to $1"; fi
  touch "$1/.bootmenu-seeded"
}

# Starts the chosen app (choice.sh is sourced) in the background; APP_PID = its pid.
launch_app() {
  app_binary || { echo "runner: ERROR $BM_ID could not be decrypted (no key on this unit, or no room)"; return 1; }
  stop_official
  log_holders "before launching $BM_ID"
  DATA=""
  if [ "$BM_DATA" != none ] && [ -n "$BM_DATA" ]; then
    DATA="$USB_RUN/$BM_DATA"
    # the official application keeps its files straight in /root/data; other apps in /root/data/<id>
    sub="/$BM_ID"; [ "$BM_OFFICIAL_BUILD" = 1 ] && sub=""
    if ! mkdir -p "$DATA$sub" 2>/dev/null; then
      DATA="$STAGE/data/$BM_ID"; mkdir -p "$DATA$sub"
      echo "runner: WARNING could not create $USB_RUN/$BM_DATA, using RAM ($DATA); data is lost on exit"
    fi
    [ "$BM_OFFICIAL_BUILD" = 1 ] && seed_official_data "$DATA"
  fi
  if [ -n "$DATA" ] && [ "$MOUNT_MODE" = global ]; then
    if mount -o bind "$DATA" /root/data; then GLOBAL_MOUNT=1
    elif mount -t tmpfs -o size=16m tmpfs /root/data; then
      GLOBAL_MOUNT=1; echo "runner: WARNING bind mount failed; /root/data is a 16 MB RAM tmpfs while $BM_ID runs"
    else
      echo "runner: ERROR could not redirect /root/data off NAND; not starting $BM_ID"; return 1
    fi
  fi
  echo "runner: launching $BM_ID: $APP_EXE${BM_ARGS:+ $BM_ARGS} (cwd $BM_DIR, /root/data = ${DATA:-unchanged}, $MOUNT_MODE)"
  (
    cd "$BM_DIR" 2>/dev/null || cd /tmp
    [ -n "$BM_CONF" ] && read_conf "$USB_RUN/$BM_CONF" "$BM_PREFIX" && echo "runner: $BM_CONF -> ${BM_PREFIX}*"
    export HOME="${DATA:-$STAGE}" BOOTMENU_STICK="$USB_RUN" BOOTMENU_APP_DIR="$BM_DIR" BOOTMENU_APP_ID="$BM_ID"
    if [ -n "$DATA" ] && [ "$MOUNT_MODE" = ns ]; then
      # shellcheck disable=SC2086
      exec unshare -m sh -c 'mount -o bind "$1" /root/data || exit 97; shift; exec "$@"' sh "$DATA" "$APP_EXE" $BM_ARGS
    fi
    # shellcheck disable=SC2086
    exec "$APP_EXE" $BM_ARGS
  ) </dev/null &
  APP_PID=$!
  return 0
}

# Runs the menu; on success choice.sh holds the choice. fb first; if that fails or crashes, x11, which falls back to
# the default entry (or the official software) by itself when it cannot draw either.
run_menu() {
  rm -f "$STAGE/choice.sh"
  echo "runner: menu${MSG:+ (message: $MSG)}"
  FALLBACK=""
  "$STAGE/bin/bootmenu" --stick "$USB_RUN" --embedded "$STAGE/apps" --out "$STAGE/choice.sh" \
    ${MSG:+--message "$MSG"} </dev/null &
  echo $! > "$STAGE/child.pid"; wait $!; rc=$?
  rm -f "$STAGE/child.pid"
  if [ $rc -eq 6 ]; then
    echo "runner: menu: the stick was pulled"; touch "$STAGE/gone"
  fi
  if [ ! -f "$STAGE/choice.sh" ] && [ ! -f "$STAGE/request" ] && [ ! -f "$STAGE/gone" ] \
     && { [ $rc -eq 2 ] || [ $rc -ge 128 ]; }; then
    echo "runner: menu: gl-fb failed (rc=$rc); trying gl-x11"
    "$STAGE/bin/bootmenu" --stick "$USB_RUN" --embedded "$STAGE/apps" --out "$STAGE/choice.sh" --render x11 \
      --fallback ${MSG:+--message "$MSG"} </dev/null &
    echo $! > "$STAGE/child.pid"; wait $!; rc=$?
    rm -f "$STAGE/child.pid"
    [ $rc -eq 3 ] && FALLBACK=1 && echo "runner: menu: no screen; the default entry was chosen without it"
  fi
  MSG=""
  echo "runner: menu exited rc=$rc"
  [ -f "$STAGE/choice.sh" ]
}

load_choice() {
  BM_KIND=""; BM_ID=""; BM_NAME=""; BM_SOURCE=""; BM_EXE=""; BM_DIR=""; BM_ARGS=""; BM_DATA=""; BM_CONF=""
  BM_PREFIX=""; BM_RUN_FROM=""; BM_SIZE=""; BM_MTIME=""; BM_ENCRYPTED=""; BM_OFFICIAL_BUILD=""
  . "$STAGE/choice.sh"
}

# --- Bring-up ----------------------------------------------------------------------------------------------------------
DISPLAY=${DISPLAY:-:0}   # the boot scripts never export it; the X server (Xfbdev) is :0 (used by gl-x11 only)
export DISPLAY
cd "$STAGE" 2>/dev/null || cd /tmp

# Network and SSH beside the menu, never in front of it.
( setup_network; start_ssh ) &

# Watcher: the stick's block device disappearing ends the session; every 5 s the alias address is put back.
DEV=$(awk -v m="$USB_RUN" '$2 == m { print $1; exit }' /proc/mounts 2>/dev/null)
echo "runner: watching stick device ${DEV:-unknown}"
(
  tick=0
  while [ ! -f "$STAGE/runner.done" ]; do
    if { [ -n "$DEV" ] && [ ! -b "$DEV" ]; } || { [ -n "$DEV" ] && ! grep -q " $USB_RUN " /proc/mounts; }; then
      echo "runner: stick removed, ending the session"
      touch "$STAGE/gone"
      p=$(cat "$STAGE/child.pid" 2>/dev/null)
      if [ -n "$p" ]; then kill -TERM "$p" 2>/dev/null; sleep 3; kill -KILL "$p" 2>/dev/null; fi
      exit 0
    fi
    tick=$((tick+1))
    if [ $((tick % 5)) -eq 0 ] && [ "${BOOTMENU_SSH:-1}" != 0 ] && [ "$SSH_IP" != none ] \
       && [ -e "/sys/class/net/$NET_IF" ] && ! alias_present; then
      add_alias && echo "runner: net: $NET_IF:16 = $SSH_IP put back"
    fi
    sleep 1
  done
) &

# --- Supervisor --------------------------------------------------------------------------------------------------------
# NEXT: menu (show it), run (resolve RUN_SPEC without the menu), app (run choice.sh), official, idle (wait for ctl).
# ctl writes $STAGE/request ("menu", "run SPEC", "official", "stop") and stops the current child.
NEXT=menu; MSG=""; RUN_SPEC=""; FALLBACK=""
while :; do
  [ -f "$STAGE/gone" ] && break
  if [ -f "$STAGE/request" ]; then
    read -r req arg < "$STAGE/request"; rm -f "$STAGE/request"
    echo "runner: ctl request: $req $arg"
    case "$req" in
      menu) NEXT=menu;; official|stock) NEXT=official;; stop) NEXT=idle;;
      run) NEXT=run; RUN_SPEC="$arg";;
    esac
  fi
  echo "$NEXT" > "$STAGE/state"
  case "$NEXT" in
    menu)
      if run_menu; then
        load_choice
        if [ "$BM_KIND" = official ]; then NEXT=official; else NEXT=app; fi
      elif [ -f "$STAGE/request" ] || [ -f "$STAGE/gone" ]; then
        continue
      else
        echo "runner: no choice from the menu; giving the unit back to the official software"
        NEXT=official
      fi
      ;;
    run)
      rm -f "$STAGE/choice.sh"
      if "$STAGE/bin/bootmenu" --stick "$USB_RUN" --embedded "$STAGE/apps" --out "$STAGE/choice.sh" \
           --resolve "$RUN_SPEC" && [ -f "$STAGE/choice.sh" ]; then
        load_choice
        if [ "$BM_KIND" = official ]; then NEXT=official; else NEXT=app; fi
      else
        echo "runner: run: no app '$RUN_SPEC'"; NEXT=idle
      fi
      ;;
    app)
      echo "app $BM_ID" > "$STAGE/state"
      t0=$(cut -d. -f1 /proc/uptime)
      if ! launch_app; then MSG="$BM_NAME could not start (see bootmenu.log)"; NEXT=menu; continue; fi
      echo "$APP_PID" > "$STAGE/child.pid"
      echo "runner: $BM_ID pid $APP_PID"
      ( sleep 5; log_holders "5 s after launching $BM_ID" ) &
      wait "$APP_PID"; rc=$?
      rm -f "$STAGE/child.pid"
      after_app
      t1=$(cut -d. -f1 /proc/uptime)
      echo "runner: $BM_ID exited rc=$rc after $((t1 - t0)) s"
      [ -f "$STAGE/gone" ] && break
      [ -f "$STAGE/request" ] && continue
      if [ "$rc" -ne 0 ] && [ $((t1 - t0)) -lt 5 ]; then
        MSG="$BM_NAME stopped right away (exit code $rc, see bootmenu.log)"
      fi
      NEXT=menu   # back to the menu when an app quits
      # ...unless the menu could not draw: then the unit goes back to the official software instead of a relaunch loop
      [ -n "$FALLBACK" ] && [ -z "$MSG" ] && NEXT=official
      ;;
    official)
      restore_official; NEXT=idle
      ;;
    idle)
      sleep 1
      ;;
  esac
done

touch "$STAGE/runner.done"
after_app
restore_official   # first: the unit is usable again at once
stop_ssh
rm -f "$STAGE/runner.pid" "$STAGE/state"
echo "runner: session over"
sync
RUNNER
  chmod 0755 "$STAGE/run.sh"

  # 3) ctl: the remote control, run over ssh (on the session PATH as `ctl`).
  cat > "$STAGE/ctl.sh" <<'CTL'
#!/bin/sh
# ctl - control the Boot menu session on the SP-16 over ssh. The runner (/tmp/bootmenu/run.sh) does the work.
STAGE=/tmp/bootmenu
LOG=$(cat "$STAGE/log.path" 2>/dev/null)
USB=$(cat "$STAGE/usb.path" 2>/dev/null)

die() { echo "ctl: $*" >&2; exit 1; }
runner_alive() { p=$(cat "$STAGE/runner.pid" 2>/dev/null); [ -n "$p" ] && kill -0 "$p" 2>/dev/null; }
usage() {
  cat <<'USAGE'
ctl status              runner state, the running app, official app, ssh, addresses
ctl menu                show the boot menu (stops whatever runs)
ctl run ID[@stick|@embedded]   start an app without the menu (ID as `ctl list` prints it)
ctl start               run the last app again
ctl official            stop the app and run the official software
ctl stop                stop the app and keep the screen empty
ctl update [ID] < BIN   put BIN on the stick as the app ID (default: the last app) and run it
ctl list                the apps the menu would list
ctl log [N]             last N lines of bootmenu.log (default 100)
ctl follow              follow the log (Ctrl-C ends)
USAGE
}

# Asks the runner for something, then stops the current child (menu or app) so the runner acts on it.
request() {
  runner_alive || die "the runner is not running (re-insert the stick)"
  echo "$*" > "$STAGE/request"
  p=$(cat "$STAGE/child.pid" 2>/dev/null)
  if [ -n "$p" ]; then
    kill -TERM "$p" 2>/dev/null
    i=0
    while [ -f "$STAGE/child.pid" ] && [ "$(cat "$STAGE/child.pid" 2>/dev/null)" = "$p" ] && [ $i -lt 10 ]; do
      sleep 1; i=$((i+1))
    done
    if [ "$(cat "$STAGE/child.pid" 2>/dev/null)" = "$p" ]; then
      echo "ctl: pid $p ignored SIGTERM, killing it"; kill -KILL "$p" 2>/dev/null
    fi
  fi
  i=0
  while [ -f "$STAGE/request" ] && [ $i -lt 15 ]; do sleep 1; i=$((i+1)); done
  echo "ctl: $* -> $(cat "$STAGE/state" 2>/dev/null)"
}

last_id() { l=$(cat "$USB/bootmenu.last" 2>/dev/null); echo "${l%@*}"; }

case "${1:-help}" in
  status)
    if runner_alive; then echo "runner    pid $(cat "$STAGE/runner.pid"), state: $(cat "$STAGE/state" 2>/dev/null)"
    else echo "runner    not running"; fi
    p=$(cat "$STAGE/child.pid" 2>/dev/null); [ -n "$p" ] && echo "child     pid $p ($(cat /proc/$p/comm 2>/dev/null))"
    s=$(pidof EP085_Main_R 2>/dev/null); echo "official app ${s:-not running}"
    d=$(cat "$STAGE/dropbear.pid" 2>/dev/null); echo "ssh       ${d:+pid $d}${d:-not running}"
    echo "last      $(cat "$USB/bootmenu.last" 2>/dev/null)"
    echo "carrier   $(cat /sys/class/net/eth0/carrier 2>/dev/null || echo '?')"
    ifconfig 2>/dev/null | awk '/^[a-z]/ { i = $1 } /inet addr:/ { sub("addr:", "", $2); print "address   " i " " $2 }'
    ;;
  menu) request menu ;;
  run) [ -n "$2" ] || die "usage: ctl run ID"; request run "$2" ;;
  start) id=$(cat "$USB/bootmenu.last" 2>/dev/null); [ -n "$id" ] || die "no last app; ctl run ID"; request run "$id" ;;
  official|stock) request official ;;
  stop) request stop ;;
  list) "$STAGE/bin/bootmenu" --stick "$USB" --embedded "$STAGE/apps" --list 2>/dev/null ;;
  update)
    [ -t 0 ] && die "send the binary on stdin: ssh root@UNIT ctl update [ID] < app"
    id=${2:-$(last_id)}
    [ -n "$id" ] && [ "$id" != official ] || die "name the app: ctl update ID < app"
    new="$STAGE/update.$$"
    cat > "$new" || { rm -f "$new"; die "upload failed (no room in /tmp?)"; }
    size=$(wc -c < "$new")
    dd if="$new" bs=4 count=1 2>/dev/null | grep -q ELF || { rm -f "$new"; die "the upload ($size bytes) is not an ELF file"; }
    # Where the stick copy lives: the app's own file when it is on the stick, else a new file beside autoexec.bin.
    target="$USB/$id"
    if "$STAGE/bin/bootmenu" --stick "$USB" --embedded "$STAGE/apps" --resolve "$id@stick" --out "$STAGE/update.choice" 2>/dev/null; then
      target=$(sed -n "s/^BM_EXE='\(.*\)'$/\1/p" "$STAGE/update.choice")
    fi
    rm -f "$STAGE/update.choice"
    echo "ctl: received $size bytes -> $target"
    if ! { cp "$new" "$target.new" && mv -f "$target.new" "$target"; }; then
      rm -f "$new" "$target.new"; die "could not write $target"
    fi
    chmod 0755 "$target" 2>/dev/null # (FAT has no modes; elsewhere run_from=stick needs it)
    rm -f "$new"; sync
    request run "$id@stick"
    ;;
  log) [ -n "$LOG" ] || die "no log path"; tail -n "${2:-100}" "$LOG" ;;
  follow) [ -n "$LOG" ] || die "no log path"; tail -f "$LOG" ;;
  help|-h|--help) usage ;;
  *) usage; exit 2 ;;
esac
CTL
  chmod 0755 "$STAGE/ctl.sh"
  ln -sf "$STAGE/ctl.sh" "$STAGE/bin/ctl"

  # 4) Detach the runner (new session, so udev tearing down this process cannot take it along): the menu starts now.
  echo "autoexec.sh: detaching runner"
  if command -v setsid >/dev/null 2>&1; then
    setsid "$STAGE/run.sh" "$LOG" "$USB" </dev/null >>"$LOG" 2>&1 &
  else
    nohup "$STAGE/run.sh" "$LOG" "$USB" </dev/null >>"$LOG" 2>&1 &
  fi

  # 5) Copy the embedded apps out of the image while the menu is already up (the hook unmounts it when we return).
  #    RAM first; if /tmp is full, onto the stick (bootmenu-cache/), linked into place.
  for d in "$SELF_DIR"/apps/*/; do
    [ -d "$d" ] || continue
    id=$(basename "$d")
    dst="$STAGE/apps/$id"
    if cp -R "$d." "$dst/" 2>/dev/null; then
      rm -f "$dst/.pending"
      echo "autoexec.sh: embedded app $id unpacked to RAM"
    else
      rm -rf "$dst"
      alt="$USB/bootmenu-cache/$id"
      rm -rf "$alt"; mkdir -p "$alt"
      if cp -R "$d." "$alt/" 2>/dev/null; then
        ln -s "$alt" "$dst"
        echo "autoexec.sh: embedded app $id unpacked to the stick ($alt; /tmp is full)"
      else
        rm -rf "$alt"
        echo "autoexec.sh: ERROR could not unpack embedded app $id"
      fi
    fi
  done
  sync
  echo "autoexec.sh: done, returning 0"
} >>"$LOG" 2>&1

exit 0
