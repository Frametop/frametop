#!/usr/bin/env bash
# Build the hand recorder's headset panel ft-handpanel in the dev container on the Frame
# (hands/rec/build/; it also runs there), or in the release image (pack/Containerfile). Like
# gaze/build.sh: the pinned public OpenVR header (scripts/openvr.sh; the DMA-BUF import is newer
# than the header shipped with SteamVR's samples), stb_truetype
# for the text and stb_image (PNG and GIF) for the pose pictures and the demo clips, both pinned and checked in scripts/stb.sh.
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
"$root/scripts/sync.sh" >/dev/null
exec "$root/scripts/frame.sh" -C hands/rec 'set -e; mkdir -p build/include
. ../../scripts/openvr.sh
. ../../scripts/stb.sh
stb_fetch build/include stb_truetype.h stb_image.h
g++ -std=c++17 -O2 -Wall -Wno-unused-parameter -Wno-missing-field-initializers -Ibuild/include $(pkg-config --cflags gbm libdrm) \
  -o build/ft-handpanel panel/ft-handpanel.cpp $OPENVR_LIBS \
  $(pkg-config --libs gbm libdrm) -lpthread
echo "built build/ft-handpanel"'
