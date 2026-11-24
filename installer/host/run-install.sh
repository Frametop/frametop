#!/usr/bin/env bash
# The Frametop Installer's service runs this: the install (get.sh --release ...), then its exit
# status in DIR/result for the window, then a SteamVR restart if you picked one. The result is
# written before the restart, which closes everything open in VR, the window included.
#
# Usage: run-install.sh DIR RESTART(0|1) -- COMMAND...
set -u
dir=$1 restart=$2
shift 3
rm -f "$dir/result"
"$@"
rc=$?
echo "$rc" >"$dir/result"
if [ "$rc" = 0 ] && [ "$restart" = 1 ]; then
  printf '\n== Restarting SteamVR (it closes everything open in VR, this window too)\n'
  sleep 3   # time for the window to show it
  systemctl --user restart steamvr.service ||
    echo "Couldn't restart SteamVR. Restart it yourself, or reboot the headset."
fi
exit "$rc"
