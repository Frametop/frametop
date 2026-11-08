#!/usr/bin/env bash
# Window drag (DEE-345, user 2026-11-07): a modifier held with a mouse button moves or resizes a
# window from anywhere in it. ft-screens does it on floating windows and PC window panels
# (screens/windowdrag.h); this sets KWin's own mouse bindings, for the windows on the desktop's
# screens, from the same setting in ~/.config/frametop.conf:
#   WINDOW_DRAG_MOD     meta (default) | alt | off
#   WINDOW_DRAG_LEFT    move (default) | resize | none
#   WINDOW_DRAG_RIGHT   resize (default) | move | none
#   WINDOW_DRAG_MIDDLE  none (default) | move | resize
#
#   session/window-drag.sh KWINRC               write them into KWINRC (the session script, before KWin starts)
#   session/window-drag.sh --live [MOD L R M]   write them into the running desktop's kwinrc (these words,
#                                               not the file's) and have KWin read it again
# A word outside those leaves KWin's bindings as they are and exits 2.
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
cfg=$HOME/.config/frametop
conf=$HOME/.config/frametop.conf

live=0
if [ "${1:-}" = --live ]; then
  live=1
  shift
  kwinrc=$cfg/kwinrc
else
  [ $# -eq 1 ] || { echo "usage: window-drag.sh KWINRC | --live [MOD LEFT RIGHT MIDDLE]" >&2; exit 2; }
  kwinrc=$1
  shift
fi

if [ $# -eq 4 ]; then
  mod=$1 left=$2 right=$3 middle=$4
elif [ $# -eq 0 ]; then
  WINDOW_DRAG_MOD=meta WINDOW_DRAG_LEFT=move WINDOW_DRAG_RIGHT=resize WINDOW_DRAG_MIDDLE=none
  # (only the file's lines in its grammar, quoted: nothing in it runs)
  if [ -f "$conf" ]; then eval "$(python3 "$here/../conf/ft_conf.py" shell "$conf")"; fi
  mod=$WINDOW_DRAG_MOD left=$WINDOW_DRAG_LEFT right=$WINDOW_DRAG_RIGHT middle=$WINDOW_DRAG_MIDDLE
else
  echo "usage: window-drag.sh KWINRC | --live [MOD LEFT RIGHT MIDDLE]" >&2
  exit 2
fi

case $mod in
  meta | super) key=Meta ;;
  alt) key=Alt ;;
  off) key=Meta ;;  # (every button does nothing, below)
  *) echo "window drag: WINDOW_DRAG_MOD is meta, alt or off, not '$mod'" >&2; exit 2 ;;
esac
action() {
  case $1 in
    move) echo Move ;;
    resize) echo Resize ;;
    none) echo Nothing ;;
    *) echo "window drag: a button does move, resize or none, not '$1'" >&2; return 2 ;;
  esac
}
a1=$(action "$left") && a2=$(action "$middle") && a3=$(action "$right") || exit 2
[ "$mod" != off ] || a1=Nothing a2=Nothing a3=Nothing

# KWin's numbering: CommandAll1 is the left button, 2 the middle, 3 the right.
kwriteconfig6 --file "$kwinrc" --group MouseBindings --key CommandAllKey "$key"
kwriteconfig6 --file "$kwinrc" --group MouseBindings --key CommandAll1 "$a1"
kwriteconfig6 --file "$kwinrc" --group MouseBindings --key CommandAll2 "$a2"
kwriteconfig6 --file "$kwinrc" --group MouseBindings --key CommandAll3 "$a3"
echo "window drag: KWin's $key+left $a1, +middle $a2, +right $a3"
[ "$live" = 1 ] || exit 0

# The desktop's D-Bus, from its Plasma shell (as decoration/apply.sh finds it).
bus=
for pid in $(pgrep -x plasmashell); do
  env=$( (tr '\0' '\n' < "/proc/$pid/environ") 2>/dev/null) || continue
  grep -qx "XDG_CONFIG_HOME=$cfg" <<<"$env" || continue
  bus=$(sed -n 's/^DBUS_SESSION_BUS_ADDRESS=//p' <<<"$env")
done
[ -n "$bus" ] || { echo "window drag: the desktop isn't running; it reads them when it starts"; exit 0; }
DBUS_SESSION_BUS_ADDRESS=$bus gdbus call --session -d org.kde.KWin -o /KWin -m org.kde.KWin.reconfigure >/dev/null
