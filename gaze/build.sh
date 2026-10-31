#!/usr/bin/env bash
# Build ft-gaze and the calibration panel ft-gazepanel in the dev container on the Frame
# (gaze/build/; they also run there). The panel draws its text with stb_truetype (public
# domain, one header, pinned and checked in scripts/stb.sh).
# The eye tracking API (IVRInput::GetEyeTrackingDataRelativeToNow) is newer than the header
# shipped with SteamVR's samples, so this uses the pinned public header (scripts/openvr.sh).
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
"$root/scripts/sync.sh" >/dev/null
exec "$root/scripts/frame.sh" -C gaze 'set -e; mkdir -p build/include
. ../scripts/openvr.sh
g++ -std=c++17 -O2 -Wall -Wno-unused-parameter -Wno-missing-field-initializers -Ibuild/include -I../pointer/common \
  -o build/ft-gaze ft-gaze.cpp $OPENVR_LIBS -lpthread
. ../scripts/stb.sh
stb_fetch build/include stb_truetype.h
g++ -std=c++17 -O2 -ffunction-sections -fdata-sections -Wl,--gc-sections -Wall -Wno-unused-parameter -Wno-missing-field-initializers -Ibuild/include $(pkg-config --cflags gbm libdrm) \
  -o build/ft-gazepanel panel/ft-gazepanel.cpp $OPENVR_LIBS \
  $(pkg-config --libs gbm libdrm) -lpthread
strip --strip-unneeded build/ft-gaze build/ft-gazepanel
echo "built build/ft-gaze build/ft-gazepanel"'
