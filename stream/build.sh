#!/usr/bin/env bash
# Build stream/build/ft-stream (ft-stream.cpp and host.cpp: a remote display's stream, for
# ft-screens) in the dev container on the Frame, and the remote-display spikes:
# stream/build/ft-dectest (spike S1, spike/ft-dectest.cpp) and stream/build/ft-streamtest
# (spike S3, spike/ft-streamtest.cpp). Uses the pinned OpenVR SDK (scripts/openvr.sh), as
# ft-screens does, for IVRIPCResourceManagerClient::ImportDmabuf. ft-stream and ft-streamtest
# also need moonlight-embedded (GPLv3) at a pinned commit: its libgamestream for pairing and
# launching, and the moonlight-common-c it pins for the protocol, and its table of Windows key
# codes. ft-stream plays the host's sound with Opus and PipeWire's PulseAudio server
# (libpulse-simple), and sends the Frame's microphone back the same way. moonlight-mic.patch adds
# that to moonlight-common-c (LiSendMicrophoneOpusData); the checkout is made again when it
# changes.
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
"$root/scripts/sync.sh" >/dev/null
exec "$root/scripts/frame.sh" -C stream 'set -e; mkdir -p build/include
. ../scripts/openvr.sh
me=f32e415aea6797d261d6b470dcf8bf18727341c2
src=build/moonlight-embedded
mlc=$src/third_party/moonlight-common-c
mark=$me-$(sha256sum moonlight-mic.patch | cut -c1-12)
if [ ! -f $src/.ft-$mark ]; then
  rm -rf $src build/mlc build/gamestream
  # Only this revision and its pinned submodules are build inputs, not their history.
  git init -q $src
  git -C $src fetch -q --depth 1 https://github.com/moonlight-stream/moonlight-embedded.git $me
  git -C $src checkout -q --detach FETCH_HEAD
  git -C $src submodule update -q --init --depth 1 third_party/moonlight-common-c
  git -C $mlc submodule update -q --init --depth 1 enet
  git -C $mlc apply "$PWD/moonlight-mic.patch"
  touch $src/.ft-$mark
fi
if [ ! -f build/mlc/libmoonlight-common-c.a ]; then
  cmake -S $mlc -B build/mlc -G Ninja -DBUILD_SHARED_LIBS=OFF -DCMAKE_BUILD_TYPE=Release >/dev/null
  ninja -C build/mlc >/dev/null
fi
mkdir -p build/gamestream
for f in client http mkcert xml; do
  [ build/gamestream/$f.o -nt $src/libgamestream/$f.c ] ||
    gcc -O2 -w -I$mlc/src -c $src/libgamestream/$f.c -o build/gamestream/$f.o
done
flags="-std=c++17 -O2 -Wall -Wno-missing-field-initializers -Ibuild/include $(pkg-config --cflags gbm libdrm egl glesv2)"
vr="$OPENVR_LIBS"
g++ $flags -o build/ft-dectest spike/ft-dectest.cpp $(pkg-config --libs gbm egl glesv2) $vr
echo "built build/ft-dectest"
g++ $flags -I$mlc/src -I$src/libgamestream -o build/ft-streamtest spike/ft-streamtest.cpp build/gamestream/*.o \
  build/mlc/libmoonlight-common-c.a build/mlc/enet/libenet.a \
  $(pkg-config --libs gbm egl glesv2 libcurl openssl expat uuid) -lpthread $vr
echo "built build/ft-streamtest"
g++ $flags -I$mlc/src -I$src/libgamestream -I$src/src -o build/ft-stream ft-stream.cpp host.cpp build/gamestream/*.o \
  build/mlc/libmoonlight-common-c.a build/mlc/enet/libenet.a \
  $(pkg-config --libs gbm egl glesv2 libcurl openssl expat uuid opus libpulse-simple) -lpthread $vr
strip --strip-unneeded build/ft-dectest build/ft-streamtest build/ft-stream
echo "built build/ft-stream"'
