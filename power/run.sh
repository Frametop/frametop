#!/usr/bin/env bash
# Install, start, stop, or inspect ft-powerd on the Frame (runs in the dev container).
# Usage: power/run.sh install|uninstall   # user service, starts with SteamVR
#        power/run.sh start|stop|restart|status|log [lines]
#        power/run.sh off|on               # the displays off now (to try it) or back on
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
. "$root/scripts/_env.sh"
. "$root/scripts/install-lib.sh"
frame="$root/scripts/frame.sh"
unit=frametop-power.service
case ${1:-status} in
  install)
    "$root/scripts/sync.sh" >/dev/null
    install_unit "$root/power/$unit"
    "$frame" --host "set -e; pkill -x ft-powerd || true
$(enable_units $unit)
$(start_with_steamvr $unit)" ;;
  uninstall) "$frame" --host "systemctl --user disable --now $unit 2>/dev/null; rm -f ~/.config/systemd/user/$unit; systemctl --user daemon-reload; echo removed" ;;
  start|stop|restart) "$frame" --host "systemctl --user $1 $unit; systemctl --user is-active $unit" ;;
  status) "$frame" --host "systemctl --user is-active $unit; python3 -c 'import socket; s=socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM); s.bind(\"\"); s.settimeout(1); s.sendto(b\"status\", \"\\0ft_powerd\"); print(s.recv(256).decode())' 2>/dev/null || echo 'ft-powerd not answering'" ;;
  log) "$frame" --host "journalctl --user -u $unit --no-pager -o cat -n ${2:-30}" ;;
  off|on) "$frame" --host "python3 -c 'import socket; s=socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM); s.bind(\"\"); s.settimeout(1); s.sendto(b\"$1\", \"\\0ft_powerd\"); print(s.recv(256).decode())'" ;;
  *) echo "usage: $0 install|uninstall|start|stop|restart|status|log|off|on" >&2; exit 2 ;;
esac
