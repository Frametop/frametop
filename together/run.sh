#!/usr/bin/env bash
# Install, turn on or off, or inspect Frametop Together on the Frame: ft-together (this
# headset's end of a shared room) and the programs that make a room here, user services that
# start and stop with it (together/README.md, "Running it").
# Usage: together/run.sh install|uninstall    # install leaves it off
#        together/run.sh on|off               # on: now and at every login (Frametop Settings does the same)
#        together/run.sh restart|status|log [lines]
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
. "$root/scripts/_env.sh"
. "$root/scripts/install-lib.sh"
frame="$root/scripts/frame.sh"
main=frametop-together.service
rooms="frametop-room-people.service frametop-room-panels.service frametop-room-voice.service frametop-room-away.service frametop-room-input.service frametop-room-points.service"
units="$main $rooms"

states="for u in $units; do echo \"\$u: \$(systemctl --user is-active \$u)\"; done"

case ${1:-status} in
  install)
    # A release brings them built (pack/Containerfile), and never builds (FRAME_RELEASE).
    [ "$FRAME_RELEASE" = 1 ] || "$root/together/build.sh"
    for u in $units; do
      install_unit "$root/together/$u"
    done
    # One that runs goes on from this folder's programs; one that is off stays off.
    "$frame" --host "systemctl --user daemon-reload; systemctl --user try-restart $main
echo \"Frametop Together: installed, \$(systemctl --user is-enabled --quiet $main 2>/dev/null && echo on || echo 'off (turn it on in Frametop Settings, or with together/run.sh on)')\"" ;;
  uninstall)
    "$frame" --host "systemctl --user disable --now $main 2>/dev/null; systemctl --user stop $rooms 2>/dev/null
for u in $units; do rm -f ~/.config/systemd/user/\$u; done; systemctl --user daemon-reload; echo removed" ;;
  on) "$frame" --host "systemctl --user enable --now $main; $states" ;;
  off) "$frame" --host "systemctl --user disable --now $main; $states" ;;
  restart) "$frame" --host "systemctl --user restart $main; $states" ;;
  status) "$frame" --host "$states; journalctl --user -u $main --no-pager -o cat -n 4" || true ;;
  log) "$frame" --host "journalctl --user $(printf -- '-u %s ' $units)--no-pager -o short -n ${2:-30}" ;;
  *) echo "usage: $0 install|uninstall|on|off|restart|status|log [lines]" >&2; exit 2 ;;
esac
