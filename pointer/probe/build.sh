#!/usr/bin/env bash
# Build the probes in the dev container on the Frame (pointer/probe/build/):
#   vrprobe    devices, roles, activity levels, charging, the dashboard's pointer device
#   lasertest  who has SteamVR's laser mouse, and how fast our device takes it back
#   ctlnoise   controller pose noise, resting and held still (the handoff arbiter's thresholds)
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
"$root/scripts/sync.sh" >/dev/null
exec "$root/scripts/frame.sh" -C pointer/probe 'set -e; mkdir -p build
. ../../scripts/openvr.sh
for p in vrprobe lasertest ctlnoise; do
  g++ -std=c++17 -O2 -Wall $OPENVR_CFLAGS -I../common -o build/$p $p.cpp $OPENVR_LIBS
done
echo "built build/vrprobe build/lasertest build/ctlnoise"'
