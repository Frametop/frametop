#!/usr/bin/env bash
# Build ft-pcsend for a split compute PC, on that PC (Linux): its streaming host runs it with
# --feed to send the PC's windows to a Frametop Together room (docs/together.md, "A PC's panels").
#
#   together/pcsend/build-host.sh OUT_DIR
#
# puts ft-pcsend and the libmsquic.so.2 it loads from its own folder in OUT_DIR. The host looks
# for it in ../libexec/vibeshine next to its own program, then in /usr/libexec/vibeshine.
#
# It needs g++, cmake, ninja, git and OpenSSL 3.5 or later (which has the QUIC API msquic
# needs). msquic is the commit together/build.sh pins: one pin for both ends of the link. Its
# build tree stays in ~/.cache/ft-pcsend-build (FT_PCSEND_WORK) for the next run.
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
out=${1:?usage: build-host.sh OUT_DIR}
work=${FT_PCSEND_WORK:-${XDG_CACHE_HOME:-$HOME/.cache}/ft-pcsend-build}
mq=$(sed -n 's/^mq=\([0-9a-f]\{40\}\).*/\1/p' "$here/../build.sh")
[ ${#mq} = 40 ] || { echo "build-host.sh: together/build.sh names no msquic commit" >&2; exit 1; }
pkg-config --atleast-version=3.5 openssl || { echo "build-host.sh: OpenSSL 3.5 or later is needed" >&2; exit 1; }

src=$work/msquic-$mq
if [ ! -f "$src/.fetched" ]; then
  # Only this revision is a build input, not its history or its submodules (OpenSSL is the PC's).
  mkdir -p "$src"
  git init -q "$src"
  git -C "$src" fetch -q --depth 1 https://github.com/microsoft/msquic.git "$mq"
  git -C "$src" checkout -q --detach FETCH_HEAD
  [ "$(git -C "$src" rev-parse HEAD)" = "$mq" ]
  touch "$src/.fetched"
fi
lib=$src/out/bin/Release/libmsquic.so.2
if [ ! -f "$lib" ]; then
  cmake -S "$src" -B "$src/out" -G Ninja -DCMAKE_BUILD_TYPE=Release -DQUIC_BUILD_SHARED=ON \
    -DQUIC_TLS_LIB=openssl -DQUIC_USE_EXTERNAL_OPENSSL=ON -DQUIC_BUILD_TOOLS=OFF -DQUIC_BUILD_TEST=OFF \
    -DQUIC_BUILD_PERF=OFF -DQUIC_ENABLE_LOGGING=OFF -DQUIC_EMBED_GIT_HASH=OFF >/dev/null
  ninja -C "$src/out" >/dev/null
fi

mkdir -p "$out"
cp "$lib" "$out/libmsquic.so.2"
cd "$here/.."
g++ -std=c++17 -O2 -Wall -Wextra -Wno-missing-field-initializers -o "$out/ft-pcsend" ft-pcsend.cpp \
  net/quiclinks.cpp net/quicframes.cpp net/quicfiles.cpp net/lookup.cpp net/tailnet.cpp net/selfcert.cpp \
  proto/messages.cpp -I"$src/src/inc" "$out/libmsquic.so.2" -Wl,-rpath,\$ORIGIN $(pkg-config --libs openssl) -lpthread
strip --strip-unneeded "$out/ft-pcsend" "$out/libmsquic.so.2"
echo "built $out/ft-pcsend"
