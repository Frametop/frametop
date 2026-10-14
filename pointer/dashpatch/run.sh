#!/usr/bin/env bash
# Install, start, stop, or inspect the dashboard patch (ft-dashpatch.py) on the Frame.
# Usage: pointer/dashpatch/run.sh install|uninstall   # user service, starts with SteamVR; install also
#                                                    # turns on SteamVR's dashboard debugger (127.0.0.1:8089)
#        pointer/dashpatch/run.sh start|stop|restart|status|log [lines]
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
. "$root/scripts/_env.sh"
frame="$root/scripts/frame.sh"
unit=frametop-dashpatch.service
case ${1:-status} in
  install)
    fill_template "$root/pointer/dashpatch/$unit" | on_frame "mkdir -p ~/.config/systemd/user && cat > ~/.config/systemd/user/$unit"
    on_frame "chmod +x pointer/dashpatch/ft-dashpatch.py"
    on_frame "/usr/bin/python3 -I pointer/dashpatch/ft-dashpatch.py on" || true
    "$frame" --host "set -e; systemctl --user daemon-reload; systemctl --user enable $unit
$(start_with_steamvr $unit)" ;;
  uninstall)
    "$frame" --host "systemctl --user disable --now $unit 2>/dev/null; rm -f ~/.config/systemd/user/$unit; systemctl --user daemon-reload; echo removed"
    on_frame "/usr/bin/python3 -I pointer/dashpatch/ft-dashpatch.py off" || true ;;
  start|stop|restart) "$frame" --host "systemctl --user $1 $unit; systemctl --user is-active $unit" ;;
  status) "$frame" --host "systemctl --user is-active $unit" || true; on_frame "/usr/bin/python3 -I pointer/dashpatch/ft-dashpatch.py status" || true ;;
  log) "$frame" --host "journalctl --user -u $unit --no-pager -o cat -n ${2:-30}" ;;
  *) echo "usage: $0 install|uninstall|start|stop|restart|status|log" >&2; exit 2 ;;
esac
