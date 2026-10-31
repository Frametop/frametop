#!/usr/bin/env bash
# A throwaway desktop for experiments, next to the running one: ft-screens --no-vr (its own
# Wayland socket, control socket @ft_screens_test, and runtime folder) with a bare nested
# KWin inside it, on its own D-Bus and config folder. Nothing touches SteamVR, the input
# relay, or the real desktop's settings. Run it from the PC (it runs itself on the Frame)
# or on the Frame. Build ft-screens first (screens/build.sh).
#
#   headless.sh start [SCREENS] [SPARES]   (default 2 screens, 1920x1080 and 1080x1920, and
#                                          3 spare outputs, disabled once KWin is up)
#   headless.sh stop
#   headless.sh ask '<ft-screens command>'  e.g. toplevels, "input 1 down 400 20"
#   headless.sh kd <kscreen-doctor args>    e.g. -o, output.WL-2.enable
#   headless.sh run <command> [args]        start an app in the test KWin (in the background)
#   headless.sh script <file.js> [name]     load a KWin script into it and run it
#   headless.sh unscript <name>
#   headless.sh js '<javascript>'           run a one-off KWin script; its print()s come back
#   headless.sh shot [OUTPUT]               a JPEG of one output (default WL-0); from the PC it
#                                          lands in captures/ and its path is printed
#   headless.sh floatd                      start ft-floatd in it (socket @frametop_float_test)
#   headless.sh float <ft-float args>       e.g. "float active", list
#   headless.sh log [kwin|screens|apps|floatd] [N] the last N lines (default 30)
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
. "$here/../../scripts/_env.sh"

if [ "$FRAME_LOCAL" != 1 ]; then
  "$REPO_ROOT/scripts/sync.sh" >/dev/null
  if [ "${1:-}" = shot ]; then
    mkdir -p "$REPO_ROOT/captures"
    out=$REPO_ROOT/captures/ft-test-${2:-WL-0}-$(date +%H%M%S).jpg
    on_frame "screens/tests/headless.sh $(printf '%q ' "$@")" > "$out"
    echo "$out"
  else
    on_frame "screens/tests/headless.sh $(printf '%q ' "$@")"
  fi
  exit
fi

rt=/run/user/$(id -u)/ft-test
cfg=/tmp/ft-test-config
bus=unix:path=$rt/bus
logs=/tmp/ft-test
mkdir -p "$logs"

kwin_env() {
  env XDG_RUNTIME_DIR="$rt" XDG_CONFIG_HOME="$cfg" DBUS_SESSION_BUS_ADDRESS="$bus" \
    WAYLAND_DISPLAY=wayland-0 QT_QPA_PLATFORM=wayland "$@"
}

ask() {
  python3 - "$1" <<'EOF'
import socket, sys
s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
s.bind("")
s.settimeout(2)
s.sendto(sys.argv[1].encode(), "\0ft_screens_test")
print(s.recv(8192).decode())
EOF
}

kd() { kwin_env kscreen-doctor "$@" 2>&1 | sed 's/\x1b\[[0-9;]*m//g'; }

stop() {
  pkill -f -- "--control ft_screens_test" 2>/dev/null || true
  [ -f "$rt/dbus.pid" ] && kill "$(cat "$rt/dbus.pid")" 2>/dev/null || true
  sleep 0.5
}

load_script() {  # file name
  local id
  id=$(kwin_env gdbus call --session -d org.kde.KWin -o /Scripting \
         -m org.kde.kwin.Scripting.loadScript "$1" "$2" | tr -dc '0-9-')
  [ "$id" -ge 0 ] 2>/dev/null || { echo "loadScript failed ($id)" >&2; return 1; }
  kwin_env gdbus call --session -d org.kde.KWin -o "/Scripting/Script$id" -m org.kde.kwin.Script.run >/dev/null
}

unload_script() {
  kwin_env gdbus call --session -d org.kde.KWin -o /Scripting -m org.kde.kwin.Scripting.unloadScript "$1" >/dev/null
}

case "${1:-}" in
  start)
    screens=${2:-2} spares=${3:-3}
    stop
    rm -rf "$rt" "$cfg"
    mkdir -p "$rt" "$cfg"
    chmod 700 "$rt"
    echo "$screens $spares" > "$rt/counts"
    args=(--screen 1920x1080@1.6 --screen 1080x1920@0.9)
    for ((i = 2; i < screens; i++)); do args+=(--screen 1920x1080@1.6); done
    args=("${args[@]:0:$((screens * 2))}")
    args+=(--spares "$spares")
    cd "$REPO_ROOT"
    nohup "$HOME/.local/bin/distrobox" enter "$FRAME_BOX" -- env XDG_RUNTIME_DIR="$rt" \
      ./screens/build/ft-screens --no-vr --socket ft-test-0 --control ft_screens_test "${args[@]}" \
      > "$logs/screens.log" 2>&1 &
    for _ in $(seq 100); do [ -S "$rt/ft-test-0" ] && break; sleep 0.2; done
    [ -S "$rt/ft-test-0" ] || { echo "ft-screens didn't start:"; tail "$logs/screens.log"; exit 1; }
    dbus-daemon --session --address="$bus" --fork --print-pid > "$rt/dbus.pid"
    # KWin's screenshot interface without a permission check, so tests can look.
    nohup env XDG_RUNTIME_DIR="$rt" XDG_CONFIG_HOME="$cfg" DBUS_SESSION_BUS_ADDRESS="$bus" \
      WAYLAND_DISPLAY=ft-test-0 KWIN_SCREENSHOT_NO_PERMISSION_CHECKS=1 QT_LOGGING_RULES="kwin_scripting=true" QT_FORCE_STDERR_LOGGING=1 \
      kwin_wayland --output-count $((screens + spares)) --width 800 --height 600 --no-lockscreen \
      > "$logs/kwin.log" 2>&1 &
    for _ in $(seq 100); do [ -S "$rt/wayland-0" ] && break; sleep 0.2; done
    [ -S "$rt/wayland-0" ] || { echo "KWin didn't start:"; tail "$logs/kwin.log"; exit 1; }
    sleep 1
    off=()
    for ((i = screens; i < screens + spares; i++)); do off+=("output.WL-$i.disable"); done
    [ ${#off[@]} -eq 0 ] || kd "${off[@]}" >/dev/null
    ask toplevels
    ;;
  stop) stop ;;
  floatd)
    pkill -f -- "--socket frametop_float_test" 2>/dev/null || true
    read -r screens spares < "$rt/counts"
    nohup env XDG_RUNTIME_DIR="$rt" XDG_CONFIG_HOME="$cfg" DBUS_SESSION_BUS_ADDRESS="$bus" \
      WAYLAND_DISPLAY=wayland-0 FT_FLOAT_DEBUG=1 "$REPO_ROOT/float/ft-floatd" --screens "$screens" --slots "$spares" \
      --control ft_screens_test --socket frametop_float_test \
      > "$logs/floatd.log" 2>&1 &
    sleep 2
    tail -3 "$logs/floatd.log"
    ;;
  float) shift; FT_FLOAT_SOCKET=frametop_float_test "$REPO_ROOT/float/ft-float" "$@" ;;
  ask) ask "$2" ;;
  kd) shift; kd "$@" ;;
  run)
    shift
    nohup env XDG_RUNTIME_DIR="$rt" XDG_CONFIG_HOME="$cfg" DBUS_SESSION_BUS_ADDRESS="$bus" \
      WAYLAND_DISPLAY=wayland-0 QT_QPA_PLATFORM=wayland "$@" >> "$logs/apps.log" 2>&1 &
    echo "started $1 (pid $!)"
    ;;
  script) load_script "$(realpath "$2")" "${3:-$(basename "$2" .js)}" ;;
  unscript) unload_script "$2" ;;
  js)
    f=$(mktemp /tmp/ft-test-XXXX.js)
    printf '%s\n' "$2" > "$f"
    name=ft-test-js-$$
    mark=$(wc -l < "$logs/kwin.log")
    load_script "$f" "$name"
    sleep 0.5
    unload_script "$name" || true
    rm -f "$f"
    tail -n +$((mark + 1)) "$logs/kwin.log" | sed -n 's/^js: //p'
    ;;
  shot)
    raw=$(mktemp /tmp/ft-test-shot.XXXXXX)
    meta=$(kwin_env python3 - "${2:-WL-0}" "$raw" <<'PY'
import os, sys, dbus
name, raw = sys.argv[1:3]
bus = dbus.bus.BusConnection(os.environ['DBUS_SESSION_BUS_ADDRESS'])
shot = dbus.Interface(bus.get_object('org.kde.KWin', '/org/kde/KWin/ScreenShot2'), 'org.kde.KWin.ScreenShot2')
r, w = os.pipe()
fd = dbus.types.UnixFd(w)
os.close(w)
opts = dbus.Dictionary({'include-cursor': True, 'native-resolution': True}, signature='sv')
res = shot.CaptureScreen(name, opts, fd)
del fd  # our copy of the write end, so the read ends when KWin is done
with os.fdopen(r, 'rb') as src, open(raw, 'wb') as dst:
    while chunk := src.read(1 << 20):
        dst.write(chunk)
print(int(res['width']), int(res['height']), int(res['stride']), int(res['format']))
PY
)
    read -r width height stride format <<<"$meta"
    layout=bgra  # QImage formats 4-6: (A)RGB32, stored B,G,R,A; 16-18: RGBA8888
    case $format in 16|17|18) layout=rgba ;; esac
    "$HOME/.local/bin/distrobox" enter "$FRAME_BOX" -- magick -size "$((stride / 4))x$height" -depth 8 \
      "$layout:$raw" -crop "${width}x${height}+0+0" +repage -alpha off -resize '1280x1280>' -quality 80 jpg:-
    rm -f "$raw"
    ;;
  log) tail -n "${3:-30}" "$logs/${2:-kwin}.log" ;;
  *) sed -n '2,20p' "$0"; exit 2 ;;
esac
