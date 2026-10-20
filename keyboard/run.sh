#!/usr/bin/env bash
# Install, start, stop, or inspect Frametop's keyboard (Full Keyboard, --frametop) on the Frame.
# Usage: keyboard/run.sh install|uninstall   # user service, starts with SteamVR
#        keyboard/run.sh start|stop|restart|status|log [lines]
# Full Keyboard's own startup unit (framekeyboard-autostart.service, from a standalone install)
# starts a copy of its own: whichever starts first runs, and the other only asks it to recenter,
# so Frametop's could lose. install says so.
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
. "$root/scripts/_env.sh"
frame="$root/scripts/frame.sh"
unit=frametop-keyboard.service
case ${1:-status} in
  install)
    "$root/scripts/sync.sh" >/dev/null
    fill_template "$root/keyboard/$unit" | on_frame "mkdir -p ~/.config/systemd/user && cat > ~/.config/systemd/user/$unit"
    "$frame" --host "set -e; pkill -x framekeyboard || true
systemctl --user daemon-reload; systemctl --user enable $unit
if systemctl --user is-enabled --quiet framekeyboard-autostart.service 2>/dev/null; then
  echo 'note: Full Keyboard is also installed on its own (framekeyboard-autostart.service). Frametop runs its own copy;'
  echo '      turn the standalone one off in its settings (Start with SteamOS), or: systemctl --user disable --now framekeyboard-autostart.service'
fi
$(start_with_steamvr $unit)" ;;
  uninstall) "$frame" --host "systemctl --user disable --now $unit 2>/dev/null; rm -f ~/.config/systemd/user/$unit; systemctl --user daemon-reload; echo removed" ;;
  start|stop|restart|status) "$frame" --host "systemctl --user $1 $unit; systemctl --user is-active $unit" || true ;;
  log) "$frame" --host "journalctl --user -u $unit --no-pager -o cat -n ${2:-30}" ;;
  *) echo "usage: $0 install|uninstall|start|stop|restart|status|log [lines]" >&2; exit 2 ;;
esac
