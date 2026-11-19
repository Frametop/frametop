#!/usr/bin/env bash
# Build ft-screens in the dev container on the Frame (screens/build/ft-screens), and
# ft-handtest, which tries the hand cutouts (handcut.cpp) on a test panel of its own, and
# ft-handview, which draws the hands when Room View is off (handview.cpp), and
# ft-spotpanel, frunner's search panel (frunner/ft-spotpanel.cpp; ft-frunner runs it), and
# ft-avatars, which draws avatar bodies (avatar/view, posed by avatar/pose; avatar/room for the
# people of a shared room): it is built here
# because it wears the hands ft-handview draws (handskin.cpp, handcut.cpp) and shows its mirror
# the way ft-handview shows them (vroutput.cpp).
# compositor.c is the wlroots side (C; wlroots headers aren't C++), vr.cpp the OpenVR side,
# remote.c the remote screens (their streams are ft-stream's, stream/build.sh), share.c the
# screens shared with a Together room (their encoders are ft-sharepanel's, together/build.sh),
# child.c how both run their programs.
# gsforward.cpp is the X side of the gamescope apps (gsforward.h), on xcb (wlroots-devel brings it);
# gslink.cpp runs it on a thread of its own.
# vrgsapps.cpp is the gamescope apps' panels, on OpenVR (vrpanel.h: what it shares with vr.cpp).
# vrsteamwin.cpp is Steam's own windows as panels (steamwin.h; STEAM_DASH_PANEL, a Display Settings switch), and
# vrsteamfollow.cpp its popups, placed with them (steamplace.h), its menu glued to its page (steamglue.h), vrsteamhit.cpp
# Steam's own (empty) overlays shrunk to nothing (steamshrink.h); steamcdp.cpp sends the laser on
# them to their pages in Steam's CEF, on a thread of its own (X's pointer doesn't reach them).
# vrtiles.cpp is the dashboard tiles' glue (the anchor, the grid's places, the focus, the memory, the tiles commands).
# vr.cpp needs OpenVR's IVRIPCResourceManagerClient (ImportDmabuf), which the header
# shipped with SteamVR on the Frame predates, so the build uses the public header from
# Valve's openvr repo (pinned in scripts/openvr.sh).
# label.cpp draws the panels' labels with stb_truetype (public domain, one header, pinned and
# checked in scripts/stb.sh);
# ft-handview --snapshot writes PNGs with stb_image_write (the same).
# ft-handview's hand models are compiled in once: tools/hand-asset.py makes build/handasset.cpp from
# screens/assets/hands each build (handasset.h, which declares it, is in the tree);
# a person's own are read at run time (handmodels.cpp, with avatar/modelfile.cpp's reader);
# its robot hand is built in code (handrobot.cpp).
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
"$root/scripts/sync.sh" >/dev/null
exec "$root/scripts/frame.sh" -C screens 'set -e; mkdir -p build/include
. ../scripts/openvr.sh
. ../scripts/stb.sh
stb_fetch build/include stb_truetype.h stb_image_write.h
cc="gcc -std=c11 -O2 -Wall -Wno-unused-parameter $(pkg-config --cflags wlroots-0.20 wayland-server xkbcommon libdrm pixman-1)"
$cc -c -o build/compositor.o compositor.c
$cc -c -o build/remote.o remote.c
$cc -c -o build/share.o share.c
$cc -c -o build/child.o child.c
cxx="g++ -std=c++17 -O2 -Wall -Wno-missing-field-initializers -Ibuild/include $(pkg-config --cflags egl glesv2 gbm libdrm)"
$cxx -c -o build/vr.o vr.cpp
$cxx -c -o build/vrgsapps.o vrgsapps.cpp
$cxx -c -o build/vrtiles.o vrtiles.cpp
$cxx -c -o build/vrbarmenu.o vrbarmenu.cpp
$cxx -c -o build/vrbarhold.o vrbarhold.cpp
$cxx -c -o build/vrbarside.o vrbarside.cpp
$cxx -c -o build/vrresetask.o vrresetask.cpp
$cxx -c -o build/vrquick.o vrquick.cpp
$cxx -c -o build/vrsteamwin.o vrsteamwin.cpp
$cxx -c -o build/vrsteamfollow.o vrsteamfollow.cpp
$cxx -c -o build/vrsteamhit.o vrsteamhit.cpp
$cxx -c -o build/label.o label.cpp
$cxx -c -o build/overlaytex.o overlaytex.cpp
$cxx -c -o build/handcut.o handcut.cpp
$cxx -c -o build/handrender.o handrender.cpp
$cxx -c -o build/rendernode.o rendernode.cpp
$cxx -c -o build/handinput.o handinput.cpp
$cxx -c -o build/palmmenu.o palmmenu.cpp
$cxx -c -o build/grabthat.o grabthat.cpp
$cxx -c -o build/steampanels.o steampanels.cpp
$cxx $(pkg-config --cflags xcb xcb-xtest xcb-icccm) -c -o build/gsforward.o gsforward.cpp
$cxx -pthread -c -o build/gslink.o gslink.cpp
$cxx -pthread -c -o build/steamcdp.o steamcdp.cpp
$cxx -c -o build/handtest.o handtest.cpp
$cxx -c -o build/handmesh.o handmesh.cpp
$cxx -c -o build/handskin.o handskin.cpp
$cxx -c -o build/handmodels.o handmodels.cpp
$cxx -c -o build/modelfile.o ../avatar/modelfile.cpp
python3 -I tools/hand-asset.py --cpp build/handasset.cpp >/dev/null
$cxx -I. -c -o build/handasset.o build/handasset.cpp
$cxx -c -o build/handrobot.o handrobot.cpp
$cxx -c -o build/vroutput.o vroutput.cpp
$cxx -c -o build/handview.o handview.cpp
$cxx -c -o build/bodyfile.o ../avatar/bodyfile.cpp
$cxx -c -o build/bodypose.o ../avatar/pose/bodypose.cpp
avatars=""
for f in bodymesh handfit demo bodygpu windowview mirrorview belowview sheet stage canvas picture live main; do
  $cxx -Wextra -I. -c -o build/avatars-$f.o ../avatar/view/$f.cpp
  avatars="$avatars build/avatars-$f.o"
done
for f in room/people room/client room/room room/sentbody room/pretend room/roompicture abstract/abstract; do   # the people of a shared room
  $cxx -Wextra -I. -c -o build/avatars-${f/\//-}.o ../avatar/$f.cpp
  avatars="$avatars build/avatars-${f/\//-}.o"
done
for f in micear ownface; do   # the face of the avatar you wear: its mouth from the microphone
  $cxx -Wextra $(pkg-config --cflags libpulse-simple) -c -o build/avatars-$f.o ../avatar/face/$f.cpp
  avatars="$avatars build/avatars-$f.o"
done
vrlibs="$(pkg-config --libs egl glesv2 gbm) $OPENVR_LIBS"
g++ -o build/ft-screens build/compositor.o build/remote.o build/share.o build/child.o build/vr.o build/vrgsapps.o build/vrtiles.o build/vrbarmenu.o build/vrbarhold.o build/vrbarside.o build/vrresetask.o build/vrquick.o build/vrsteamwin.o build/vrsteamfollow.o build/vrsteamhit.o build/label.o build/overlaytex.o build/handcut.o build/handrender.o build/rendernode.o build/handinput.o build/palmmenu.o build/grabthat.o build/steampanels.o build/gsforward.o build/gslink.o build/steamcdp.o \
  $(pkg-config --libs wlroots-0.20 wayland-server xkbcommon pixman-1 xcb xcb-xtest xcb-icccm) $vrlibs -pthread
g++ -o build/ft-handtest build/handtest.o build/handcut.o build/handrender.o build/rendernode.o $vrlibs
g++ -o build/ft-handview build/handview.o build/vroutput.o build/handmesh.o build/handskin.o build/handmodels.o build/modelfile.o build/handasset.o build/handrobot.o build/handcut.o build/handrender.o build/rendernode.o $vrlibs
$cxx -o build/ft-spotpanel ../frunner/ft-spotpanel.cpp $(pkg-config --libs gbm libdrm) $OPENVR_LIBS -lpthread
g++ -o build/ft-avatars $avatars build/bodypose.o build/bodyfile.o build/modelfile.o build/handskin.o build/handmodels.o build/handasset.o build/handmesh.o build/handcut.o build/handrender.o build/rendernode.o build/vroutput.o $vrlibs $(pkg-config --libs libpulse-simple) -lpthread
strip --strip-unneeded build/ft-screens build/ft-handtest build/ft-handview build/ft-spotpanel build/ft-avatars
echo "built build/ft-screens build/ft-handtest build/ft-handview build/ft-spotpanel build/ft-avatars"'
