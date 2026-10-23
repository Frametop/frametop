#!/usr/bin/env bash
# Fetch the two STB headers used by the countdown renderer. Same upstream commit
# as screens/build.sh, with hashes so the source and image builds verify downloads.
# Also usable on a development PC for the fake-OpenVR tests; no Frame or VR access.
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
include="$here/build/include"
stb=2c980bb59875b0d32144a71867fbdebb2f77cd20
mkdir -p "$include"

digest() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1"
  else
    shasum -a 256 "$1"
  fi
}

fetch() {
  local name=$1 wanted=$2 actual file="$include/$1" part="$include/$1.part.$$"
  if [ -f "$file" ]; then
    actual=$(digest "$file")
    [ "${actual%% *}" = "$wanted" ] && return 0
  fi
  if ! curl -fsSL "https://raw.githubusercontent.com/nothings/stb/$stb/$name" -o "$part"; then
    rm -f "$part"
    return 1
  fi
  actual=$(digest "$part")
  if [ "${actual%% *}" != "$wanted" ]; then
    echo "$name from stb $stb does not match its pinned sha256" >&2
    rm -f "$part"
    return 1
  fi
  mv "$part" "$file"
}

fetch stb_truetype.h ecd30b05e0dd4fea3a13c26810dd9e1992dc379049482c393d5a19e6b5090aab
fetch stb_image_write.h cbd5f0ad7a9cf4468affb36354a1d2338034f2c12473cf1a8e32053cb6914a05
