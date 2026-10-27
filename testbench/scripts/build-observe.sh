#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
mkdir -p "$ROOT/build"
# No SteamVR link-time dependency, EGL, GL, image library, or private ABI.
"${CXX:-g++}" -std=c++17 -O2 -Wall -Wextra -Wpedantic \
    "$ROOT/native/observe.cpp" -ldl -pthread -o "$ROOT/build/frame-observe"
