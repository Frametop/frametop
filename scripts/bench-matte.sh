#!/usr/bin/env bash
# The cutout composite's cost on the GPU, offline (screens/tests/matte_bench.cpp): recorded hands
# (ft-handreplay --publish-dump) cut out of a test buffer, the hand-shaped matte against the
# capsules. Shows nothing in the headset; it does use the GPU for a few seconds.
#   scripts/bench-matte.sh DUMP [--frames N] [--panel-px W H]
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
args=$(printf ' %q' "$@")
"$root/scripts/frame.sh" -C screens "set -e; mkdir -p build/include; . ../scripts/openvr.sh
g++ -std=c++17 -O2 -Wall -Wno-missing-field-initializers -Ibuild/include \$(pkg-config --cflags egl glesv2 gbm libdrm) \
  tests/matte_bench.cpp handcut.cpp handrender.cpp -o build/matte-bench \$(pkg-config --libs egl glesv2 gbm) \$OPENVR_LIBS
build/matte-bench$args"
