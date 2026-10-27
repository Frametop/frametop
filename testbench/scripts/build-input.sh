#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
out=${1:-"$root/build"}
mkdir -p -- "$out"
out=$(cd -- "$out" && pwd)
CXX=${CXX:-g++}
flags=(-std=c++17 -O2 -g -fPIC -fvisibility=hidden -Wall -Wextra -Werror -pthread -isystem "$root/vendor")
# Pass optional compiler flags as an array using conventional shell splitting.
read -r -a extra <<< "${CXXFLAGS:-}"
"$CXX" "${flags[@]}" "${extra[@]}" -shared -static-libstdc++ -static-libgcc -Wl,--exclude-libs,ALL -Wl,--version-script="$root/native/input.exports" "$root/native/input_proxy.cpp" "$root/native/input_state.cpp" "$root/native/control.cpp" -ldl -o "$out/libframe_input.so"
"$CXX" "${flags[@]}" "${extra[@]}" -shared -static-libstdc++ -static-libgcc -Wl,--exclude-libs,ALL -Wl,--version-script="$root/native/interpose.exports" "$root/native/interpose.cpp" -ldl -o "$out/libframe_loader.so"
printf 'Built %s/libframe_input.so and %s/libframe_loader.so\n' "$out" "$out"
