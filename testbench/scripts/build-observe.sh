#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
OUT=${1:-"$ROOT/build"}
mkdir -p "$OUT"
# Fetches the pinned OpenVR headers into build/include.
(cd "$ROOT" && . ../scripts/openvr.sh)
# No SteamVR link-time dependency, EGL, GL, image library, or private ABI.
"${CXX:-g++}" -std=c++17 -O2 -Wall -Wextra -Wpedantic -isystem "$ROOT/build/include" -static-libstdc++ -static-libgcc \
    "$ROOT/native/observe.cpp" -ldl -pthread -o "$OUT/frame-observe"
