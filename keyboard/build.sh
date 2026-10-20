#!/usr/bin/env bash
# Build Full Keyboard (TaiKeid's, vendored here: FRAMETOP.md) in the dev container on the
# Frame: build/frametop/framekeyboard, which frametop-keyboard.service runs with --frametop.
# It finds its layouts, themes and Steam bridge beside itself, in build/frametop. Like
# Frametop's other OpenVR programs it loads SteamVR's own libopenvr_api first. Its tests:
# FULL_KEYBOARD_TESTS=1 keyboard/build.sh (ctest in build/frametop).
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
"$root/scripts/sync.sh" >/dev/null
exec "$root/scripts/frame.sh" -C keyboard "set -e
. ../scripts/openvr.sh
tests=${FULL_KEYBOARD_TESTS:-0}
cmake -S . -B build/frametop -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DBUILD_TESTING=\$([ \$tests = 1 ] && echo ON || echo OFF) \
  -DOPENVR_LIBRARY=\$openvr_lib/libopenvr_api.so -DOPENVR_RUNTIME_PATH=\$openvr_rpath >/dev/null
cmake --build build/frametop
[ \$tests = 1 ] && (cd build/frametop && XDG_RUNTIME_DIR=\${XDG_RUNTIME_DIR:-/run/user/\$(id -u)} ctest --output-on-failure)
echo 'built build/frametop/framekeyboard'"
