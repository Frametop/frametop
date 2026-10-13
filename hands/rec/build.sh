#!/usr/bin/env bash
# Build the hand recorder's headset panel ft-handpanel in the dev container on the Frame
# (hands/rec/build/; it also runs there). Like gaze/build.sh: the pinned public OpenVR header
# (the DMA-BUF import is newer than the header shipped with SteamVR's samples), stb_truetype
# for the text and stb_image (PNG and GIF) for the pose pictures and the demo clips, at the same stb commit.
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
"$root/scripts/sync.sh" >/dev/null
exec "$root/scripts/frame.sh" -C hands/rec 'set -e; mkdir -p build/include
openvr=v2.15.6
[ -f build/include/openvr-$openvr ] || { curl -fsSL "https://raw.githubusercontent.com/ValveSoftware/openvr/$openvr/headers/openvr.h" -o build/include/openvr.h && touch build/include/openvr-$openvr; }
stb=2c980bb59875b0d32144a71867fbdebb2f77cd20
[ -f build/include/stb-$stb ] || { curl -fsSL "https://raw.githubusercontent.com/nothings/stb/$stb/stb_truetype.h" -o build/include/stb_truetype.h && touch build/include/stb-$stb; }
[ -f build/include/stb_image-$stb ] || { curl -fsSL "https://raw.githubusercontent.com/nothings/stb/$stb/stb_image.h" -o build/include/stb_image.h && touch build/include/stb_image-$stb; }
g++ -std=c++17 -O2 -Wall -Wno-unused-parameter -Wno-missing-field-initializers -Ibuild/include $(pkg-config --cflags gbm libdrm) \
  -o build/ft-handpanel panel/ft-handpanel.cpp -L/opt/steamvr/bin/linuxarm64 -lopenvr_api -Wl,-rpath,/opt/steamvr/bin/linuxarm64 \
  $(pkg-config --libs gbm libdrm) -lpthread
echo "built build/ft-handpanel"'
