#!/usr/bin/env bash
# Build ft-screens in the dev container on the Frame (screens/build/ft-screens), and
# ft-handtest, which tries the hand cutouts (handcut.cpp) on a test panel of its own, and
# ft-handview, which draws the hands when Room View is off (handview.cpp), and
# ft-spotpanel, spotwatch's search panel (spotwatch/ft-spotpanel.cpp; ft-spotwatch runs it).
# compositor.c is the wlroots side (C; wlroots headers aren't C++), vr.cpp the OpenVR side,
# remote.c the remote screens (their streams are ft-stream's, stream/build.sh).
# vr.cpp needs OpenVR's IVRIPCResourceManagerClient (ImportDmabuf), which the header
# shipped with SteamVR on the Frame predates, so the build uses the public header from
# Valve's openvr repo (pinned in scripts/openvr.sh).
# keyboard.cpp draws its key labels with stb_truetype (public domain, one header, pinned);
# ft-handview --snapshot writes PNGs with stb_image_write (the same).
# ft-handview's hand models are compiled in (handasset.h, from screens/assets/hands by tools/hand-asset.py);
# its robot hand is built in code (handrobot.cpp).
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
"$root/scripts/sync.sh" >/dev/null
exec "$root/scripts/frame.sh" -C screens 'set -e; mkdir -p build/include
. ../scripts/openvr.sh
stb=2c980bb59875b0d32144a71867fbdebb2f77cd20
[ -f build/include/stb-$stb ] && [ -f build/include/stb_image_write.h ] || { for h in stb_truetype.h stb_image_write.h; do curl -fsSL "https://raw.githubusercontent.com/nothings/stb/$stb/$h" -o build/include/$h || exit 1; done; touch build/include/stb-$stb; }
cc="gcc -std=c11 -O2 -Wall -Wno-unused-parameter $(pkg-config --cflags wlroots-0.20 wayland-server xkbcommon libdrm pixman-1)"
$cc -c -o build/compositor.o compositor.c
$cc -c -o build/remote.o remote.c
cxx="g++ -std=c++17 -O2 -Wall -Wno-missing-field-initializers -Ibuild/include $(pkg-config --cflags egl glesv2 gbm libdrm)"
$cxx -c -o build/vr.o vr.cpp
$cxx -c -o build/keyboard.o keyboard.cpp
$cxx -c -o build/overlaytex.o overlaytex.cpp
$cxx -c -o build/handcut.o handcut.cpp
$cxx -c -o build/handrender.o handrender.cpp
$cxx -c -o build/handinput.o handinput.cpp
$cxx -c -o build/palmmenu.o palmmenu.cpp
$cxx -c -o build/grabthat.o grabthat.cpp
$cxx -c -o build/steampanels.o steampanels.cpp
$cxx -c -o build/handtest.o handtest.cpp
$cxx -c -o build/handmesh.o handmesh.cpp
$cxx -c -o build/handskin.o handskin.cpp
$cxx -c -o build/handrobot.o handrobot.cpp
$cxx -c -o build/handview.o handview.cpp
vrlibs="$(pkg-config --libs egl glesv2 gbm) $OPENVR_LIBS"
g++ -o build/ft-screens build/compositor.o build/remote.o build/vr.o build/keyboard.o build/overlaytex.o build/handcut.o build/handrender.o build/handinput.o build/palmmenu.o build/grabthat.o build/steampanels.o \
  $(pkg-config --libs wlroots-0.20 wayland-server xkbcommon pixman-1) $vrlibs
g++ -o build/ft-handtest build/handtest.o build/handcut.o build/handrender.o $vrlibs
g++ -o build/ft-handview build/handview.o build/handmesh.o build/handskin.o build/handrobot.o build/handcut.o build/handrender.o $vrlibs
$cxx -o build/ft-spotpanel ../spotwatch/ft-spotpanel.cpp $(pkg-config --libs gbm libdrm) $OPENVR_LIBS -lpthread
strip --strip-unneeded build/ft-screens build/ft-handtest build/ft-handview build/ft-spotpanel
echo "built build/ft-screens build/ft-handtest build/ft-handview build/ft-spotpanel"'
