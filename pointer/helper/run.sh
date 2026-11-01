#!/usr/bin/env bash
# Start, stop, or inspect the ft-pointer helper on the Frame (runs in the dev container).
# Usage: pointer/helper/run.sh install|uninstall   # user service, starts with SteamVR
#        pointer/helper/run.sh start|stop|restart|status|log [lines]
# With the service installed, start/stop/restart/log go through systemd.
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
. "$root/scripts/_env.sh"
. "$root/scripts/install-lib.sh"
frame="$root/scripts/frame.sh"
unit=frametop-pointer.service
installed() { "$frame" --host "test -f ~/.config/systemd/user/$unit" 2>/dev/null; }
case ${1:-status} in
  install)
    "$root/scripts/sync.sh" >/dev/null
    install_unit "$root/pointer/helper/$unit"
    "$frame" --host "set -e; pkill -x ft-pointer || true
$(enable_units $unit)
$(start_with_steamvr $unit)" ;;
  uninstall) "$frame" --host "systemctl --user disable --now $unit 2>/dev/null; rm -f ~/.config/systemd/user/$unit; systemctl --user daemon-reload; echo removed" ;;
  start|stop|restart) if installed; then "$frame" --host "systemctl --user $1 $unit; systemctl --user is-active $unit"; exit; fi ;;&
  log) if installed; then "$frame" --host "journalctl --user -u $unit --no-pager -o cat -n ${2:-30}"; exit; fi ;;&
  start) "$frame" -C pointer/helper 'pgrep -x ft-pointer >/dev/null && { echo "already running"; exit 0; }
nohup ./build/ft-pointer > /tmp/ft-pointer.log 2>&1 &
sleep 2; pgrep -ax ft-pointer; cat /tmp/ft-pointer.log' ;;
  stop) "$frame" --host 'pkill -x ft-pointer && echo stopped || echo "not running"' ;;
  restart) "$0" stop; sleep 1; exec "$0" start ;;
  status) "$frame" --host 'pgrep -ax ft-pointer || echo "not running"' ;;
  log) "$frame" --host "tail -n ${2:-30} /tmp/ft-pointer.log" ;;
  *) echo "usage: $0 start|stop|restart|status|log" >&2; exit 2 ;;
esac
