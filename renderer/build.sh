#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p build/include
. ../scripts/openvr.sh
. ../scripts/stb.sh
stb_fetch build/include stb_image_write.h
python3 -I tools/hand-asset.py --cpp build/handasset.cpp >/dev/null
cxx=${CXX:-g++}
"$cxx" -std=c++17 -O2 -Wall -I. -Ibuild/include -o build/handview-test \
    tests/handview_test.cpp handmesh.cpp handskin.cpp build/handasset.cpp \
    handrobot.cpp handcut.cpp
build/handview-test
read -r -a cflags <<< "$(pkg-config --cflags egl glesv2 gbm libdrm)"
read -r -a libs <<< "$(pkg-config --libs egl glesv2 gbm libdrm)"
read -r -a openvr_libs <<< "$OPENVR_LIBS"
"$cxx" -std=c++17 -O2 -Wall -Wno-missing-field-initializers -I. -Ibuild/include \
    "${cflags[@]}" -o build/framehands-view \
    handview.cpp handmesh.cpp handskin.cpp build/handasset.cpp handrobot.cpp \
    handcut.cpp eyes.cpp rendernode.cpp "${libs[@]}" "${openvr_libs[@]}"
strip --strip-unneeded build/framehands-view
