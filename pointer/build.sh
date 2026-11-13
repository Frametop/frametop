#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p build/include
. ../scripts/openvr.sh
cxx=${CXX:-g++}
"$cxx" -std=c++17 -O2 -fPIC -shared -fvisibility=hidden -fno-math-errno \
  -static-libstdc++ -static-libgcc -Wall -Wno-unused-parameter -Ibuild/include \
  -o build/driver_fh_pointer.so driver/driver_fh_pointer.cpp -lpthread
read -r -a openvr_libs <<< "$OPENVR_LIBS"
"$cxx" -std=c++17 -O2 -Wall -Ibuild/include -I../renderer \
  -o build/fh-input helper/fh-input.cpp ../renderer/handcut.cpp "${openvr_libs[@]}" -lpthread
strip --strip-unneeded build/driver_fh_pointer.so build/fh-input
