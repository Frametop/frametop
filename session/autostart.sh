#!/usr/bin/env bash
# Starts the Frametop desktop when SteamVR starts, if AUTOSTART=1 in ~/.config/frametop.conf
# (Start with SteamVR, on the Power tab of Frametop Display Settings). frametop-autostart.service
# runs it each time SteamVR starts; the Frame starts SteamVR at boot, so that covers boot too.
# FT_CONF names another settings file, and FT_AUTOSTART_DRY=1 stops before starting (tests).
set -u

here=$(dirname "$(readlink -f "$0")")
conf=${FT_CONF:-$HOME/.config/frametop.conf}

# The last AUTOSTART= line wins, as when the session sources the file.
value=$(sed -n 's/^[[:space:]]*AUTOSTART=\([^[:space:]#]*\).*/\1/p' "$conf" 2>/dev/null | tail -n 1)
if [ "${value:-0}" != 1 ]; then
  echo "AUTOSTART is off: not starting the desktop"
  exit 0
fi
[ "${FT_AUTOSTART_DRY:-0}" = 1 ] && { echo "AUTOSTART is on: would start the desktop"; exit 0; }

# ft-screens is an overlay app: it needs SteamVR's server and compositor up. The unit starts
# as soon as SteamVR's does, while vrstartup is still bringing them up.
for _ in $(seq 180); do
  pgrep -x vrserver >/dev/null && pgrep -x vrcompositor >/dev/null && break
  sleep 1
done
if ! { pgrep -x vrserver >/dev/null && pgrep -x vrcompositor >/dev/null; }; then
  echo "SteamVR didn't come up within 3 minutes: not starting the desktop" >&2
  exit 1
fi
# The dashboard and its drivers come up just after the compositor.
sleep 5
echo "AUTOSTART is on: starting the desktop"
exec "$here/../desktops.sh" start
