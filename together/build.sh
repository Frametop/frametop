#!/usr/bin/env bash
# Build Frametop Together's programs (docs/together.md, together/README.md) in the dev container
# on the Frame:
#   together/build/ft-together    this Frame's end of a session: the links and the session's rules
#   together/build/ft-sharepanel  encodes one screen this Frame shares, for ft-together to send
#                                 (ft-screens runs it: screens/share.c)
#   together/build/ft-peerpanel   decodes one panel someone else shares, for ft-screens to show
#                                 (ft-screens runs it: screens/remote.c); its decoder is the
#                                 remote displays' (stream/iris.h), whose header wants OpenVR's
#   together/build/ft-roomvoice  the room's voices: sends the wearer's to the others and plays
#                                 theirs, each from where their head is (Opus, PulseAudio)
#   together/build/ft-roompoints  draws where the others point, on the panels they point at
#                                 (SteamVR overlays)
# ft-roompanels, which keeps the two panel programs in step with the room, is a script
# (together/ft-roompanels).
# and with --spikes, the programs that measured what the design stands on:
#   together/build/ft-enctest   spikes T1, T2 and T4 (spike/ft-enctest.cpp on media/irisenc.cpp):
#                               the hardware encoder under several sessions
#   together/build/ft-quictest  spike T3 (spike/ft-quictest.cpp): video as QUIC datagrams
# The links are msquic (MIT) at a pinned commit, built here as a shared library on the
# container's OpenSSL (3.5 or later, which has the QUIC API msquic needs) and loaded from the
# program's own folder.
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
"$root/scripts/sync.sh" >/dev/null
spikes=0
[ "${1:-}" = --spikes ] && spikes=1
exec "$root/scripts/frame.sh" -C together "spikes=$spikes"'; set -e; mkdir -p build
flags="-std=c++17 -O2 -Wall -Wextra -Wno-missing-field-initializers"
mq=16ca4d8d785d34fb22285e7c652e477ac16475a5  # v2.6.3
src=build/msquic
if [ ! -f $src/.ft-$mq ]; then
  rm -rf $src build/msquic-out
  # Only this revision is a build input, not its history or its submodules (OpenSSL comes
  # from the container).
  git init -q $src
  git -C $src fetch -q --depth 1 https://github.com/microsoft/msquic.git $mq
  git -C $src checkout -q --detach FETCH_HEAD
  touch $src/.ft-$mq
fi
if [ ! -f build/libmsquic.so.2 ]; then
  cmake -S $src -B build/msquic-out -G Ninja -DCMAKE_BUILD_TYPE=Release -DQUIC_BUILD_SHARED=ON \
    -DQUIC_TLS_LIB=openssl -DQUIC_USE_EXTERNAL_OPENSSL=ON -DQUIC_BUILD_TOOLS=OFF -DQUIC_BUILD_TEST=OFF \
    -DQUIC_BUILD_PERF=OFF -DQUIC_ENABLE_LOGGING=OFF -DQUIC_EMBED_GIT_HASH=OFF >/dev/null
  ninja -C build/msquic-out >/dev/null
  cp build/msquic-out/bin/Release/libmsquic.so.2 build/libmsquic.so.2
  strip --strip-unneeded build/libmsquic.so.2
fi
quic="-I$src/src/inc build/libmsquic.so.2 -Wl,-rpath,\$ORIGIN"
g++ $flags -o build/ft-together ft-together.cpp net/quiclinks.cpp net/quicframes.cpp net/quicfiles.cpp net/lookup.cpp \
  net/tailnet.cpp net/selfcert.cpp session/session.cpp session/wearer.cpp session/commands.cpp proto/messages.cpp flow/video.cpp \
  flow/looks.cpp flow/control.cpp local/watchers.cpp local/media.cpp local/wardrobe.cpp local/inputs.cpp local/controlask.cpp \
  ../avatar/modelfile.cpp $quic $(pkg-config --libs openssl) -lpthread
gpu="gbm libdrm egl glesv2"
g++ $flags $(pkg-config --cflags $gpu) -o build/ft-sharepanel ft-sharepanel.cpp media/irisenc.cpp media/pictures.cpp \
  media/blit.cpp media/panelenc.cpp media/screenfeed.cpp $(pkg-config --libs $gpu) -lpthread
. ../scripts/openvr.sh
g++ $flags -Wno-unused-function -Wno-unused-parameter $OPENVR_CFLAGS $(pkg-config --cflags $gpu) -o build/ft-peerpanel \
  ft-peerpanel.cpp $(pkg-config --libs $gpu) $OPENVR_LIBS -lpthread
g++ $flags $(pkg-config --cflags opus libpulse-simple) -o build/ft-roomvoice ft-roomvoice.cpp voice/codec.cpp voice/voices.cpp \
  voice/sender.cpp voice/player.cpp voice/headask.cpp voice/pulse.cpp ../avatar/room/client.cpp ../avatar/room/people.cpp \
  $(pkg-config --libs opus libpulse-simple) -lpthread
g++ $flags $OPENVR_CFLAGS -o build/ft-roompoints ft-roompoints.cpp points/asker.cpp points/dots.cpp $OPENVR_LIBS
strip --strip-unneeded build/ft-together build/ft-sharepanel build/ft-peerpanel build/ft-roomvoice build/ft-roompoints
[ $spikes = 1 ] || { echo "built build/ft-together build/ft-sharepanel build/ft-peerpanel build/ft-roomvoice build/ft-roompoints"; exit 0; }
g++ $flags $(pkg-config --cflags gbm) -o build/ft-enctest spike/ft-enctest.cpp media/irisenc.cpp \
  $(pkg-config --libs gbm) -lpthread
g++ $flags -o build/ft-quictest spike/ft-quictest.cpp net/selfcert.cpp $quic \
  $(pkg-config --libs openssl) -lpthread
strip --strip-unneeded build/ft-enctest build/ft-quictest
echo "built build/ft-together build/ft-sharepanel build/ft-peerpanel build/ft-roomvoice build/ft-roompoints build/ft-enctest build/ft-quictest"'
